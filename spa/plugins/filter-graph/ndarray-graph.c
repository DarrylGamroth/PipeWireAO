/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/filter-graph/ndarray-graph.h>
#include <spa/param/ndarray.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/pod/iter.h>
#include <spa/utils/json.h>
#include <spa/utils/overflow.h>
#include <spa/utils/string.h>

struct fgn_node;

struct fgn_storage {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	void *memory;
};

struct fgn_port {
	struct fgn_node *node;
	const struct spa_fgn_port_info *info;
	const struct spa_fgn_format *format;
	struct fgn_port *source;
	uint32_t n_consumers;
	uint32_t external;
	struct fgn_storage storage;
	struct spa_buffer *cycle_buffer;
};

struct fgn_node {
	char *name;
	void *library;
	const struct spa_fgn_plugin *plugin;
	const struct spa_fgn_descriptor *descriptor;
	void *instance;
	struct fgn_port *ports;
	uint32_t n_inputs;
	uint32_t n_outputs;
	struct fgn_port **inputs;
	struct fgn_port **outputs;
	struct spa_fgn_buffer *process_inputs;
	struct spa_fgn_buffer *process_outputs;
	struct spa_fgn_property_info *properties;
	uint32_t n_properties;
	uint32_t indegree;
};

struct fgn_link {
	struct fgn_port *output;
	struct fgn_port *input;
};

struct spa_fgn_graph {
	struct fgn_node **nodes;
	uint32_t n_nodes;
	struct fgn_link *links;
	uint32_t n_links;
	struct fgn_node **order;
	struct fgn_port **inputs;
	uint32_t n_inputs;
	struct fgn_port **outputs;
	uint32_t n_outputs;
	bool active;
};

struct fgn_transaction {
	struct fgn_node *node;
	struct spa_fgn_property *properties;
	uint32_t n_properties;
	void *prepared;
};

static int validate_buffer(struct spa_buffer *buffer,
		const struct spa_fgn_format *format, bool output);

static bool string_equal(const char *a, const char *b)
{
	return a == b || (a != NULL && b != NULL && spa_streq(a, b));
}

static int copy_string(char *destination, size_t size, const char *source)
{
	size_t length;

	if (destination == NULL || size == 0 || source == NULL ||
	    (length = strlen(source)) >= size)
		return -ENOSPC;
	memcpy(destination, source, length + 1);
	return 0;
}

static int format_size(const struct spa_fgn_format *format, size_t *size)
{
	size_t elements = 1, bytes;
	uint32_t element_size, i;

	if (format == NULL || size == NULL || format->shape == NULL ||
	    format->n_dimensions == 0 ||
	    format->n_dimensions > SPA_NDARRAY_MAX_DIMENSIONS)
		return -EINVAL;
	if (format->layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR &&
	    format->layout != SPA_NDARRAY_LAYOUT_COLUMN_MAJOR)
		return -EINVAL;
	if ((format->rate_num == 0) != (format->rate_denom == 0))
		return -EINVAL;
	if ((element_size = spa_element_type_size(format->element_type)) == 0)
		return -EINVAL;

	for (i = 0; i < format->n_dimensions; i++) {
		if (format->shape[i] == 0 || format->shape[i] > INT32_MAX)
			return -EINVAL;
		if (spa_overflow_mul(elements, format->shape[i], &elements))
			return -EOVERFLOW;
	}
	if (spa_overflow_mul(elements, element_size, &bytes) || bytes > UINT32_MAX)
		return -EOVERFLOW;
	*size = bytes;
	return 0;
}

static bool format_equal(const struct spa_fgn_format *a,
		const struct spa_fgn_format *b)
{
	uint32_t i;

	if (a == NULL || b == NULL || a->element_type != b->element_type ||
	    a->layout != b->layout || a->rate_num != b->rate_num ||
	    a->rate_denom != b->rate_denom ||
	    a->n_dimensions != b->n_dimensions ||
	    !string_equal(a->schema, b->schema) ||
	    !string_equal(a->profile, b->profile))
		return false;
	for (i = 0; i < a->n_dimensions; i++)
		if (a->shape[i] != b->shape[i])
			return false;
	return true;
}

static int storage_init(struct fgn_storage *storage,
		const struct spa_fgn_format *format)
{
	size_t size, contiguous;
	uint32_t contiguous_axis, element_size;
	int res;

	if ((res = format_size(format, &size)) < 0)
		return res;
	element_size = spa_element_type_size(format->element_type);
	contiguous_axis = format->layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR
		? format->n_dimensions - 1 : 0;
	contiguous = (size_t)format->shape[contiguous_axis] * element_size;
	if (contiguous > INT32_MAX)
		return -EOVERFLOW;
	if ((res = posix_memalign(&storage->memory, 64, size)) != 0)
		return -res;
	memset(storage->memory, 0, size);

	storage->chunk.offset = 0;
	storage->chunk.size = size;
	storage->chunk.stride = (int32_t)contiguous;
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.flags = SPA_DATA_FLAG_READWRITE;
	storage->data.fd = -1;
	storage->data.maxsize = size;
	storage->data.data = storage->memory;
	storage->data.chunk = &storage->chunk;

	storage->header.pts = SPA_TIME_INVALID;
	storage->metas[0] = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(storage->header),
		.data = &storage->header,
	};
	spa_meta_acquisition_init(&storage->acquisition);
	storage->metas[1] = (struct spa_meta) {
		.type = SPA_META_Acquisition,
		.size = sizeof(storage->acquisition),
		.data = &storage->acquisition,
	};
	storage->buffer.n_metas = SPA_N_ELEMENTS(storage->metas);
	storage->buffer.metas = storage->metas;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
	return 0;
}

static void storage_clear(struct fgn_storage *storage)
{
	free(storage->memory);
	storage->memory = NULL;
}

static char *copy_json_value(struct spa_json *json, const char *value)
{
	struct spa_json item = SPA_JSON_START(json, value);
	const char *token;
	char *result;
	int len;

	if ((len = spa_json_next(&item, &token)) <= 0)
		return NULL;
	if (spa_json_is_container(token, len) &&
	    (len = spa_json_container_len(&item, token, len)) <= 0)
		return NULL;
	if ((result = malloc((size_t)len + 1)) == NULL)
		return NULL;
	if (spa_json_parse_stringn(token, len, result, len + 1) <= 0) {
		free(result);
		return NULL;
	}
	return result;
}

static struct fgn_node *find_node(const struct spa_fgn_graph *graph,
		const char *name)
{
	uint32_t i;

	for (i = 0; i < graph->n_nodes; i++)
		if (spa_streq(graph->nodes[i]->name, name))
			return graph->nodes[i];
	return NULL;
}

static struct fgn_port *find_port(struct fgn_node *node, const char *name,
		uint32_t direction)
{
	uint32_t i, port_id = SPA_ID_INVALID;

	if (node == NULL || name == NULL)
		return NULL;
	spa_atou32(name, &port_id, 0);
	for (i = 0; i < node->descriptor->n_ports; i++) {
		struct fgn_port *port = &node->ports[i];
		if (port->info->direction == direction &&
		    (spa_streq(port->info->name, name) ||
		     port->info->index == port_id))
			return port;
	}
	return NULL;
}

static struct fgn_port *resolve_port(const struct spa_fgn_graph *graph,
		const char *reference, uint32_t direction)
{
	char node_name[256], port_name[256];
	const char *colon;
	size_t node_len;
	struct fgn_node *node;

	if (reference == NULL || (colon = strchr(reference, ':')) == NULL)
		return NULL;
	node_len = (size_t)(colon - reference);
	if (node_len == 0 || node_len >= sizeof(node_name) ||
	    copy_string(port_name, sizeof(port_name), colon + 1) < 0)
		return NULL;
	memcpy(node_name, reference, node_len);
	node_name[node_len] = '\0';
	if ((node = find_node(graph, node_name)) == NULL)
		return NULL;
	return find_port(node, port_name, direction);
}

static const struct spa_fgn_property_info *find_property(
		const struct fgn_node *node, const char *name)
{
	uint32_t i;

	for (i = 0; i < node->n_properties; i++)
		if (spa_streq(node->properties[i].name, name))
			return &node->properties[i];
	return NULL;
}

static int validate_value(const struct spa_fgn_property_info *info,
		const struct spa_fgn_value *value)
{
	if (info == NULL || value == NULL ||
	    value->type != info->default_value.type)
		return -EINVAL;
	if (!(info->flags & SPA_FGN_PROPERTY_FLAG_RANGE))
		return 0;

	switch (value->type) {
	case SPA_TYPE_Int:
		return value->value.integer < info->minimum.value.integer ||
			value->value.integer > info->maximum.value.integer ? -ERANGE : 0;
	case SPA_TYPE_Long:
		return value->value.long_integer < info->minimum.value.long_integer ||
			value->value.long_integer > info->maximum.value.long_integer ? -ERANGE : 0;
	case SPA_TYPE_Float:
		return !isfinite(value->value.float_value) ||
			value->value.float_value < info->minimum.value.float_value ||
			value->value.float_value > info->maximum.value.float_value ? -ERANGE : 0;
	case SPA_TYPE_Double:
		return !isfinite(value->value.double_value) ||
			value->value.double_value < info->minimum.value.double_value ||
			value->value.double_value > info->maximum.value.double_value ? -ERANGE : 0;
	case SPA_TYPE_Id:
		return value->value.id < info->minimum.value.id ||
			value->value.id > info->maximum.value.id ? -ERANGE : 0;
	default:
		return -EINVAL;
	}
}

static bool value_type_supported(uint32_t type)
{
	switch (type) {
	case SPA_TYPE_Bool:
	case SPA_TYPE_Int:
	case SPA_TYPE_Long:
	case SPA_TYPE_Float:
	case SPA_TYPE_Double:
	case SPA_TYPE_Id:
	case SPA_TYPE_String:
		return true;
	default:
		return false;
	}
}

static int validate_property_info(const struct spa_fgn_property_info *info)
{
	if (info->struct_size < sizeof(*info) || info->name == NULL ||
	    info->name[0] == '\0' ||
	    (info->flags & ~(SPA_FGN_PROPERTY_FLAG_READONLY |
			SPA_FGN_PROPERTY_FLAG_RANGE)) != 0 ||
	    !value_type_supported(info->default_value.type))
		return -EINVAL;
	if (!(info->flags & SPA_FGN_PROPERTY_FLAG_RANGE))
		return 0;
	if (info->minimum.type != info->default_value.type ||
	    info->maximum.type != info->default_value.type)
		return -EINVAL;
	switch (info->default_value.type) {
	case SPA_TYPE_Int:
		if (info->minimum.value.integer > info->maximum.value.integer)
			return -EINVAL;
		break;
	case SPA_TYPE_Long:
		if (info->minimum.value.long_integer > info->maximum.value.long_integer)
			return -EINVAL;
		break;
	case SPA_TYPE_Float:
		if (!isfinite(info->minimum.value.float_value) ||
		    !isfinite(info->maximum.value.float_value) ||
		    info->minimum.value.float_value > info->maximum.value.float_value)
			return -EINVAL;
		break;
	case SPA_TYPE_Double:
		if (!isfinite(info->minimum.value.double_value) ||
		    !isfinite(info->maximum.value.double_value) ||
		    info->minimum.value.double_value > info->maximum.value.double_value)
			return -EINVAL;
		break;
	case SPA_TYPE_Id:
		if (info->minimum.value.id > info->maximum.value.id)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	return validate_value(info, &info->default_value);
}

static int value_from_pod(const struct spa_pod *pod, struct spa_fgn_value *value)
{
	const char *string;
	bool boolean;
	int res;

	if (pod == NULL || value == NULL)
		return -EINVAL;
	memset(value, 0, sizeof(*value));
	value->type = pod->type;
	switch (pod->type) {
	case SPA_TYPE_Bool:
		if ((res = spa_pod_get_bool(pod, &boolean)) < 0)
			return res;
		value->value.boolean = boolean;
		return 0;
	case SPA_TYPE_Int:
		return spa_pod_get_int(pod, &value->value.integer);
	case SPA_TYPE_Long:
		return spa_pod_get_long(pod, &value->value.long_integer);
	case SPA_TYPE_Float:
		return spa_pod_get_float(pod, &value->value.float_value);
	case SPA_TYPE_Double:
		return spa_pod_get_double(pod, &value->value.double_value);
	case SPA_TYPE_Id:
		return spa_pod_get_id(pod, &value->value.id);
	case SPA_TYPE_String:
		if (spa_pod_get_string(pod, &string) < 0)
			return -EINVAL;
		value->value.string = string;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static void build_value(struct spa_pod_builder *builder,
		const struct spa_fgn_value *value)
{
	switch (value->type) {
	case SPA_TYPE_Bool:
		spa_pod_builder_bool(builder, value->value.boolean != 0);
		break;
	case SPA_TYPE_Int:
		spa_pod_builder_int(builder, value->value.integer);
		break;
	case SPA_TYPE_Long:
		spa_pod_builder_long(builder, value->value.long_integer);
		break;
	case SPA_TYPE_Float:
		spa_pod_builder_float(builder, value->value.float_value);
		break;
	case SPA_TYPE_Double:
		spa_pod_builder_double(builder, value->value.double_value);
		break;
	case SPA_TYPE_Id:
		spa_pod_builder_id(builder, value->value.id);
		break;
	case SPA_TYPE_String:
		spa_pod_builder_string(builder, value->value.string ?: "");
		break;
	default:
		spa_pod_builder_none(builder);
		break;
	}
}

static int value_from_json(const struct spa_fgn_property_info *info,
		const char *token, int len, struct spa_fgn_value *value,
		char *string, size_t string_size)
{
	int integer;
	char number[128], *end;
	bool boolean;

	memset(value, 0, sizeof(*value));
	value->type = info->default_value.type;
	switch (value->type) {
	case SPA_TYPE_Bool:
		if (spa_json_parse_bool(token, len, &boolean) <= 0)
			return -EINVAL;
		value->value.boolean = boolean;
		break;
	case SPA_TYPE_Int:
	case SPA_TYPE_Id:
		if (spa_json_parse_int(token, len, &integer) <= 0 ||
		    (integer < 0 && value->type == SPA_TYPE_Id))
			return -EINVAL;
		if (value->type == SPA_TYPE_Int)
			value->value.integer = integer;
		else
			value->value.id = (uint32_t)integer;
		break;
	case SPA_TYPE_Float:
		if (spa_json_parse_float(token, len, &value->value.float_value) <= 0)
			return -EINVAL;
		break;
	case SPA_TYPE_Long:
		if (len <= 0 || (size_t)len >= sizeof(number))
			return -EINVAL;
		memcpy(number, token, len);
		number[len] = '\0';
		errno = 0;
		value->value.long_integer = strtoll(number, &end, 0);
		if (errno != 0 || end != number + len)
			return -EINVAL;
		break;
	case SPA_TYPE_Double:
		if (len <= 0 || (size_t)len >= sizeof(number))
			return -EINVAL;
		memcpy(number, token, len);
		number[len] = '\0';
		errno = 0;
		value->value.double_value = strtod(number, &end);
		if (errno != 0 || end != number + len)
			return -EINVAL;
		break;
	case SPA_TYPE_String:
		if (spa_json_parse_stringn(token, len, string, string_size) <= 0)
			return -EINVAL;
		value->value.string = string;
		break;
	default:
		return -ENOTSUP;
	}
	return validate_value(info, value);
}

static int node_set_initial_props(struct fgn_node *node, struct spa_json *json)
{
	struct spa_fgn_property *properties = NULL;
	char **owned_strings = NULL;
	uint32_t n_properties = 0;
	char key[256], string[512];
	const char *token;
	void *prepared = NULL;
	int len, res = 0;

	while ((len = spa_json_object_next(json, key, sizeof(key), &token)) > 0) {
		const struct spa_fgn_property_info *info = find_property(node, key);
		struct spa_fgn_property *tmp;
		char **new_strings;

		if (info == NULL || (info->flags & SPA_FGN_PROPERTY_FLAG_READONLY)) {
			res = -ENOENT;
			goto done;
		}
		if ((tmp = realloc(properties,
				(sizeof(*properties) * (n_properties + 1)))) == NULL) {
			res = -ENOMEM;
			goto done;
		}
		properties = tmp;
		if ((new_strings = realloc(owned_strings,
				(sizeof(*owned_strings) * (n_properties + 1)))) == NULL) {
			res = -ENOMEM;
			goto done;
		}
		owned_strings = new_strings;
		owned_strings[n_properties] = NULL;
		properties[n_properties].id = info->id;
		if ((res = value_from_json(info, token, len,
				&properties[n_properties].value,
				string, sizeof(string))) < 0)
			goto done;
		if (properties[n_properties].value.type == SPA_TYPE_String) {
			if ((owned_strings[n_properties] = strdup(string)) == NULL) {
				res = -ENOMEM;
				goto done;
			}
			properties[n_properties].value.value.string =
					owned_strings[n_properties];
		}
		n_properties++;
	}
	if (n_properties == 0)
		goto done;
	if (node->descriptor->prepare_props == NULL ||
	    node->descriptor->commit_props == NULL) {
		res = -ENOTSUP;
		goto done;
	}
	if ((res = node->descriptor->prepare_props(node->instance, properties,
			n_properties, &prepared)) < 0)
		goto done;
	node->descriptor->commit_props(node->instance, prepared);
	prepared = NULL;
done:
	if (prepared != NULL && node->descriptor->discard_props != NULL)
		node->descriptor->discard_props(node->instance, prepared);
	while (n_properties > 0)
		free(owned_strings[--n_properties]);
	free(owned_strings);
	free(properties);
	return res;
}

static void node_free(struct fgn_node *node)
{
	uint32_t i;

	if (node == NULL)
		return;
	if (node->instance != NULL && node->descriptor != NULL &&
	    node->descriptor->cleanup != NULL)
		node->descriptor->cleanup(node->instance);
	if (node->ports != NULL)
		for (i = 0; i < node->descriptor->n_ports; i++)
			if (node->ports[i].info != NULL &&
			    node->ports[i].info->direction == SPA_FGN_PORT_OUTPUT)
				storage_clear(&node->ports[i].storage);
	if (node->library != NULL)
		dlclose(node->library);
	free(node->properties);
	free(node->process_outputs);
	free(node->process_inputs);
	free(node->outputs);
	free(node->inputs);
	free(node->ports);
	free(node->name);
	free(node);
}

static int load_node(struct spa_fgn_graph *graph, struct spa_json *json)
{
	struct spa_json props;
	bool have_props = false;
	char key[256], name[256] = "", plugin_path[1024] = "", label[256] = "";
	const char *token;
	char *config = NULL;
	struct fgn_node *node = NULL;
	spa_fgn_plugin_entry_func_t entry;
	void *symbol;
	uint32_t i, n_input = 0, n_output = 0;
	int len, res = 0;

	while ((len = spa_json_object_next(json, key, sizeof(key), &token)) > 0) {
		if (spa_streq(key, "name")) {
			if (spa_json_parse_stringn(token, len, name, sizeof(name)) <= 0) {
				res = -EINVAL;
				goto error;
			}
		} else if (spa_streq(key, "plugin")) {
			if (spa_json_parse_stringn(token, len, plugin_path,
					sizeof(plugin_path)) <= 0) {
				res = -EINVAL;
				goto error;
			}
		} else if (spa_streq(key, "label")) {
			if (spa_json_parse_stringn(token, len, label, sizeof(label)) <= 0) {
				res = -EINVAL;
				goto error;
			}
		} else if (spa_streq(key, "type")) {
			char type[64];
			if (spa_json_parse_stringn(token, len, type, sizeof(type)) <= 0 ||
			    !spa_streq(type, "ndarray")) {
				res = -EINVAL;
				goto error;
			}
		} else if (spa_streq(key, "config")) {
			if ((config = copy_json_value(json, token)) == NULL) {
				res = -EINVAL;
				goto error;
			}
		} else if (spa_streq(key, "props")) {
			if (!spa_json_is_object(token, len)) {
				res = -EINVAL;
				goto error;
			}
			spa_json_enter(json, &props);
			have_props = true;
		} else {
			res = -EINVAL;
			goto error;
		}
	}
	if (name[0] == '\0' || plugin_path[0] == '\0' || label[0] == '\0' ||
	    find_node(graph, name) != NULL) {
		res = -EINVAL;
		goto error;
	}
	if ((node = calloc(1, sizeof(*node))) == NULL ||
	    (node->name = strdup(name)) == NULL) {
		res = -ENOMEM;
		goto error;
	}
	if ((node->library = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL)) == NULL) {
		res = -ENOENT;
		goto error;
	}
	dlerror();
	symbol = dlsym(node->library, SPA_FGN_PLUGIN_ENTRY_NAME);
	memcpy(&entry, &symbol, sizeof(entry));
	if (entry == NULL || dlerror() != NULL ||
	    (node->plugin = entry(SPA_FGN_PLUGIN_ABI_VERSION)) == NULL ||
	    node->plugin->struct_size < sizeof(*node->plugin) ||
	    node->plugin->abi_version != SPA_FGN_PLUGIN_ABI_VERSION ||
	    node->plugin->find_descriptor == NULL ||
	    (node->descriptor = node->plugin->find_descriptor(label)) == NULL ||
	    node->descriptor->struct_size < sizeof(*node->descriptor) ||
	    node->descriptor->version != SPA_FGN_PLUGIN_ABI_VERSION ||
	    node->descriptor->ports == NULL || node->descriptor->instantiate == NULL ||
	    node->descriptor->cleanup == NULL ||
	    node->descriptor->get_port_format == NULL ||
	    (node->descriptor->enum_prop_info != NULL &&
	     node->descriptor->get_prop == NULL) ||
	    ((node->descriptor->prepare_props != NULL ||
	      node->descriptor->commit_props != NULL ||
	      node->descriptor->discard_props != NULL) &&
	     (node->descriptor->prepare_props == NULL ||
	      node->descriptor->commit_props == NULL ||
	      node->descriptor->discard_props == NULL)) ||
	    ((node->descriptor->prepare_parameter != NULL ||
	      node->descriptor->commit_parameter != NULL ||
	      node->descriptor->discard_parameter != NULL) &&
	     (node->descriptor->prepare_parameter == NULL ||
	      node->descriptor->commit_parameter == NULL ||
	      node->descriptor->discard_parameter == NULL)) ||
	    node->descriptor->process == NULL) {
		res = -ENOTSUP;
		goto error;
	}
	if ((res = node->descriptor->instantiate(node->descriptor,
			config ?: "{}", &node->instance)) < 0 || node->instance == NULL)
		goto error;

	if ((node->ports = calloc(node->descriptor->n_ports,
			sizeof(*node->ports))) == NULL) {
		res = -ENOMEM;
		goto error;
	}
	for (i = 0; i < node->descriptor->n_ports; i++) {
		const struct spa_fgn_port_info *info = &node->descriptor->ports[i];
		struct fgn_port *port = &node->ports[i];
		uint32_t j;

		if (info->struct_size < sizeof(*info) || info->name == NULL ||
		    info->name[0] == '\0' ||
		    (info->flags & ~(SPA_FGN_PORT_FLAG_OPTIONAL |
				SPA_FGN_PORT_FLAG_PARAMETER)) != 0 ||
		    ((info->flags & SPA_FGN_PORT_FLAG_PARAMETER) &&
		     (info->direction != SPA_FGN_PORT_INPUT ||
		      !(info->flags & SPA_FGN_PORT_FLAG_OPTIONAL) ||
		      node->descriptor->prepare_parameter == NULL)) ||
		    (info->direction != SPA_FGN_PORT_INPUT &&
		     info->direction != SPA_FGN_PORT_OUTPUT)) {
			res = -EINVAL;
			goto error;
		}
		for (j = 0; j < i; j++)
			if (node->descriptor->ports[j].index == info->index ||
			    spa_streq(node->descriptor->ports[j].name, info->name)) {
				res = -EEXIST;
				goto error;
			}
		port->node = node;
		port->info = info;
		port->external = SPA_ID_INVALID;
		if ((res = node->descriptor->get_port_format(node->instance,
				info->index, &port->format)) < 0 ||
		    (res = format_size(port->format, &(size_t){ 0 })) < 0)
			goto error;
		if (info->direction == SPA_FGN_PORT_INPUT)
			n_input++;
		else
			n_output++;
	}
	node->n_inputs = n_input;
	node->n_outputs = n_output;
	if ((n_input > 0 &&
	     ((node->inputs = calloc(n_input, sizeof(*node->inputs))) == NULL ||
	      (node->process_inputs = calloc(n_input,
			 sizeof(*node->process_inputs))) == NULL)) ||
	    (n_output > 0 &&
	     ((node->outputs = calloc(n_output, sizeof(*node->outputs))) == NULL ||
	      (node->process_outputs = calloc(n_output,
			  sizeof(*node->process_outputs))) == NULL))) {
		res = -ENOMEM;
		goto error;
	}
	n_input = n_output = 0;
	for (i = 0; i < node->descriptor->n_ports; i++) {
		struct fgn_port *port = &node->ports[i];
		if (port->info->direction == SPA_FGN_PORT_INPUT)
			node->inputs[n_input++] = port;
		else {
			node->outputs[n_output++] = port;
			if ((res = storage_init(&port->storage, port->format)) < 0)
				goto error;
		}
	}
	if (node->descriptor->enum_prop_info != NULL) {
		for (i = 0; ; i++) {
			struct spa_fgn_property_info info = { .struct_size = sizeof(info) };
			struct spa_fgn_property_info *tmp;
			uint32_t j;

			res = node->descriptor->enum_prop_info(node->instance, i, &info);
			if (res == 0)
				break;
			if (res < 0 || (res = validate_property_info(&info)) < 0) {
				res = res < 0 ? res : -EINVAL;
				goto error;
			}
			for (j = 0; j < node->n_properties; j++)
				if (node->properties[j].id == info.id ||
				    spa_streq(node->properties[j].name, info.name)) {
					res = -EEXIST;
					goto error;
				}
			if ((tmp = realloc(node->properties,
					(sizeof(*tmp) * (node->n_properties + 1)))) == NULL) {
				res = -ENOMEM;
				goto error;
			}
			node->properties = tmp;
			node->properties[node->n_properties++] = info;
		}
	}
	if (have_props && (res = node_set_initial_props(node, &props)) < 0)
		goto error;
	{
		struct fgn_node **tmp = realloc(graph->nodes,
				(sizeof(*tmp) * (graph->n_nodes + 1)));
		if (tmp == NULL) {
			res = -ENOMEM;
			goto error;
		}
		graph->nodes = tmp;
		graph->nodes[graph->n_nodes++] = node;
	}
	free(config);
	return 0;
error:
	free(config);
	node_free(node);
	return res;
}

static int load_link(struct spa_fgn_graph *graph, struct spa_json *json)
{
	char key[256], output[512] = "", input[512] = "";
	const char *token;
	struct fgn_port *out_port, *in_port;
	struct fgn_link *links;
	int len;

	while ((len = spa_json_object_next(json, key, sizeof(key), &token)) > 0) {
		char *target;
		if (spa_streq(key, "output"))
			target = output;
		else if (spa_streq(key, "input"))
			target = input;
		else
			return -EINVAL;
		if (spa_json_parse_stringn(token, len, target, 512) <= 0)
			return -EINVAL;
	}
	if ((out_port = resolve_port(graph, output, SPA_FGN_PORT_OUTPUT)) == NULL ||
	    (in_port = resolve_port(graph, input, SPA_FGN_PORT_INPUT)) == NULL)
		return -ENOENT;
	if (in_port->info->flags & SPA_FGN_PORT_FLAG_PARAMETER)
		return -ENOTSUP;
	if (in_port->source != NULL || in_port->external != SPA_ID_INVALID)
		return -EBUSY;
	if (!format_equal(out_port->format, in_port->format))
		return -EINVAL;
	if ((links = realloc(graph->links,
			(sizeof(*links) * (graph->n_links + 1)))) == NULL)
		return -ENOMEM;
	graph->links = links;
	graph->links[graph->n_links++] = (struct fgn_link) {
		.output = out_port,
		.input = in_port,
	};
	in_port->source = out_port;
	out_port->n_consumers++;
	in_port->node->indegree++;
	return 0;
}

static int load_external_ports(struct spa_fgn_graph *graph, struct spa_json *json,
		uint32_t direction)
{
	struct fgn_port ***ports = direction == SPA_FGN_PORT_INPUT
		? &graph->inputs : &graph->outputs;
	uint32_t *n_ports = direction == SPA_FGN_PORT_INPUT
		? &graph->n_inputs : &graph->n_outputs;
	char reference[512];

	while (spa_json_get_string(json, reference, sizeof(reference)) > 0) {
		struct fgn_port *port = resolve_port(graph, reference, direction);
		struct fgn_port **tmp;

		if (port == NULL || port->external != SPA_ID_INVALID ||
		    (direction == SPA_FGN_PORT_INPUT && port->source != NULL))
			return -EINVAL;
		if ((tmp = realloc(*ports, sizeof(*tmp) * (*n_ports + 1))) == NULL)
			return -ENOMEM;
		*ports = tmp;
		port->external = *n_ports;
		(*ports)[(*n_ports)++] = port;
	}
	return 0;
}

static int load_default_ports(struct spa_fgn_graph *graph)
{
	struct fgn_node *first = graph->nodes[0];
	struct fgn_node *last = graph->nodes[graph->n_nodes - 1];
	uint32_t i;

	if (graph->n_inputs == 0)
		for (i = 0; i < first->n_inputs; i++) {
			struct fgn_port *port = first->inputs[i];
			struct fgn_port **tmp;
			if (port->source != NULL)
				continue;
			if ((tmp = realloc(graph->inputs,
					sizeof(*tmp) * (graph->n_inputs + 1))) == NULL)
				return -ENOMEM;
			graph->inputs = tmp;
			port->external = graph->n_inputs;
			graph->inputs[graph->n_inputs++] = port;
		}
	if (graph->n_outputs == 0)
		for (i = 0; i < last->n_outputs; i++) {
			struct fgn_port *port = last->outputs[i];
			struct fgn_port **tmp;
			if ((tmp = realloc(graph->outputs,
					sizeof(*tmp) * (graph->n_outputs + 1))) == NULL)
				return -ENOMEM;
			graph->outputs = tmp;
			port->external = graph->n_outputs;
			graph->outputs[graph->n_outputs++] = port;
		}
	return 0;
}

static int sort_graph(struct spa_fgn_graph *graph)
{
	uint32_t *indegree, done = 0, i, j;
	bool progress;

	if ((graph->order = calloc(graph->n_nodes, sizeof(*graph->order))) == NULL ||
	    (indegree = calloc(graph->n_nodes, sizeof(*indegree))) == NULL) {
		free(indegree);
		return -ENOMEM;
	}
	for (i = 0; i < graph->n_nodes; i++)
		indegree[i] = graph->nodes[i]->indegree;
	do {
		progress = false;
		for (i = 0; i < graph->n_nodes; i++) {
			struct fgn_node *node = graph->nodes[i];
			bool already = false;
			if (indegree[i] != 0)
				continue;
			for (j = 0; j < done; j++)
				already |= graph->order[j] == node;
			if (already)
				continue;
			graph->order[done++] = node;
			progress = true;
			for (j = 0; j < graph->n_links; j++) {
				struct fgn_link *link = &graph->links[j];
				uint32_t k;
				if (link->output->node != node)
					continue;
				for (k = 0; k < graph->n_nodes; k++)
					if (graph->nodes[k] == link->input->node &&
					    indegree[k] > 0)
						indegree[k]--;
			}
		}
	} while (progress && done < graph->n_nodes);
	free(indegree);
	return done == graph->n_nodes ? 0 : -ELOOP;
}

static int validate_graph(const struct spa_fgn_graph *graph)
{
	uint32_t i, j;

	if (graph->n_inputs == 0 || graph->n_outputs == 0)
		return -EINVAL;
	for (i = 0; i < graph->n_nodes; i++) {
		const struct fgn_node *node = graph->nodes[i];
		for (j = 0; j < node->n_inputs; j++) {
			const struct fgn_port *port = node->inputs[j];
			if (port->source == NULL && port->external == SPA_ID_INVALID &&
			    !(port->info->flags & SPA_FGN_PORT_FLAG_OPTIONAL))
				return -ENOTCONN;
		}
	}
	return 0;
}

int spa_fgn_graph_new(const char *config, struct spa_fgn_graph **result)
{
	struct spa_fgn_graph *graph;
	struct spa_json top, child, nodes = { 0 }, links = { 0 };
	struct spa_json inputs = { 0 }, outputs = { 0 };
	bool have_nodes = false, have_links = false;
	bool have_inputs = false, have_outputs = false;
	char key[256];
	const char *token;
	int len, res;

	if (config == NULL || result == NULL)
		return -EINVAL;
	*result = NULL;
	if ((graph = calloc(1, sizeof(*graph))) == NULL)
		return -ENOMEM;
	if (spa_json_begin_object(&top, config, strlen(config)) <= 0) {
		res = -EINVAL;
		goto error;
	}
	while ((len = spa_json_object_next(&top, key, sizeof(key), &token)) > 0) {
		struct spa_json *target;
		bool *present;
		char expected;

		if (spa_streq(key, "nodes")) {
			target = &nodes; present = &have_nodes; expected = '[';
		} else if (spa_streq(key, "links")) {
			target = &links; present = &have_links; expected = '[';
		} else if (spa_streq(key, "inputs")) {
			target = &inputs; present = &have_inputs; expected = '[';
		} else if (spa_streq(key, "outputs")) {
			target = &outputs; present = &have_outputs; expected = '[';
		} else {
			res = -EINVAL;
			goto error;
		}
		if (*present || token[0] != expected) {
			res = -EINVAL;
			goto error;
		}
		spa_json_enter(&top, target);
		*present = true;
	}
	if (!have_nodes) {
		res = -EINVAL;
		goto error;
	}
	while (spa_json_enter_object(&nodes, &child) > 0)
		if ((res = load_node(graph, &child)) < 0)
			goto error;
	if (graph->n_nodes == 0) {
		res = -EINVAL;
		goto error;
	}
	if (have_links)
		while (spa_json_enter_object(&links, &child) > 0)
			if ((res = load_link(graph, &child)) < 0)
				goto error;
	if (have_inputs && (res = load_external_ports(graph, &inputs,
			SPA_FGN_PORT_INPUT)) < 0)
		goto error;
	if (have_outputs && (res = load_external_ports(graph, &outputs,
			SPA_FGN_PORT_OUTPUT)) < 0)
		goto error;
	if ((!have_inputs || !have_outputs) && (res = load_default_ports(graph)) < 0)
		goto error;
	if ((res = validate_graph(graph)) < 0 || (res = sort_graph(graph)) < 0)
		goto error;
	*result = graph;
	return 0;
error:
	spa_fgn_graph_free(graph);
	return res;
}

void spa_fgn_graph_free(struct spa_fgn_graph *graph)
{
	uint32_t i;

	if (graph == NULL)
		return;
	spa_fgn_graph_deactivate(graph);
	for (i = graph->n_nodes; i > 0; i--)
		node_free(graph->nodes[i - 1]);
	free(graph->outputs);
	free(graph->inputs);
	free(graph->order);
	free(graph->links);
	free(graph->nodes);
	free(graph);
}

uint32_t spa_fgn_graph_get_n_inputs(const struct spa_fgn_graph *graph)
{
	return graph != NULL ? graph->n_inputs : 0;
}

uint32_t spa_fgn_graph_get_n_outputs(const struct spa_fgn_graph *graph)
{
	return graph != NULL ? graph->n_outputs : 0;
}

int spa_fgn_graph_get_port_format(const struct spa_fgn_graph *graph,
		uint32_t direction, uint32_t port,
		const struct spa_fgn_format **format)
{
	if (graph == NULL || format == NULL)
		return -EINVAL;
	if (direction == SPA_FGN_PORT_INPUT) {
		if (port >= graph->n_inputs)
			return -ENOENT;
		*format = graph->inputs[port]->format;
	} else if (direction == SPA_FGN_PORT_OUTPUT) {
		if (port >= graph->n_outputs)
			return -ENOENT;
		*format = graph->outputs[port]->format;
	} else {
		return -EINVAL;
	}
	return 0;
}

int spa_fgn_graph_get_port_info(const struct spa_fgn_graph *graph,
		uint32_t direction, uint32_t port, const char **node_name,
		const struct spa_fgn_port_info **info)
{
	struct fgn_port *graph_port;

	if (graph == NULL || node_name == NULL || info == NULL)
		return -EINVAL;
	if (direction == SPA_FGN_PORT_INPUT) {
		if (port >= graph->n_inputs)
			return -ENOENT;
		graph_port = graph->inputs[port];
	} else if (direction == SPA_FGN_PORT_OUTPUT) {
		if (port >= graph->n_outputs)
			return -ENOENT;
		graph_port = graph->outputs[port];
	} else {
		return -EINVAL;
	}
	*node_name = graph_port->node->name;
	*info = graph_port->info;
	return 0;
}

static int property_at(struct spa_fgn_graph *graph, uint32_t index,
		struct fgn_node **node, const struct spa_fgn_property_info **info)
{
	uint32_t i;

	for (i = 0; i < graph->n_nodes; i++) {
		if (index < graph->nodes[i]->n_properties) {
			*node = graph->nodes[i];
			*info = &graph->nodes[i]->properties[index];
			return 0;
		}
		index -= graph->nodes[i]->n_properties;
	}
	return -ENOENT;
}

int spa_fgn_graph_enum_prop_info(struct spa_fgn_graph *graph, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	struct fgn_node *node;
	const struct spa_fgn_property_info *info;
	struct spa_pod_frame object, choice;
	struct spa_pod *pod;
	char name[512];
	uint32_t flags;
	int res;

	if (graph == NULL || builder == NULL)
		return -EINVAL;
	if ((res = property_at(graph, index, &node, &info)) < 0)
		return res == -ENOENT ? 0 : res;
	if (spa_scnprintf(name, sizeof(name), "%s:%s", node->name, info->name) < 0)
		return -ENOSPC;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(builder,
			SPA_PROP_INFO_name, SPA_POD_String(name),
			SPA_PROP_INFO_description,
			SPA_POD_String(info->description ?: ""),
			0);
	flags = info->flags & SPA_FGN_PROPERTY_FLAG_READONLY
		? SPA_POD_PROP_FLAG_READONLY : 0;
	spa_pod_builder_prop(builder, SPA_PROP_INFO_type, flags);
	if (info->flags & SPA_FGN_PROPERTY_FLAG_RANGE) {
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Range, 0);
		build_value(builder, &info->default_value);
		build_value(builder, &info->minimum);
		build_value(builder, &info->maximum);
		spa_pod_builder_pop(builder, &choice);
	} else {
		build_value(builder, &info->default_value);
	}
	spa_pod_builder_add(builder,
			SPA_PROP_INFO_params, SPA_POD_Bool(true), 0);
	pod = spa_pod_builder_pop(builder, &object);
	if (pod == NULL)
		return -ENOSPC;
	if (param != NULL)
		*param = pod;
	return 1;
}

int spa_fgn_graph_get_props(struct spa_fgn_graph *graph,
		struct spa_pod_builder *builder, struct spa_pod **props)
{
	struct spa_pod_frame object, values;
	struct spa_pod *pod;
	uint32_t i, j;
	int res;

	if (graph == NULL || builder == NULL)
		return -EINVAL;
	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(builder, &values);
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->nodes[i];
		for (j = 0; j < node->n_properties; j++) {
			struct spa_fgn_value value;
			char name[512];
			if (node->descriptor->get_prop == NULL ||
			    (res = node->descriptor->get_prop(node->instance,
					node->properties[j].id, &value)) < 0)
				return node->descriptor->get_prop == NULL ? -ENOTSUP : res;
			spa_scnprintf(name, sizeof(name), "%s:%s",
					node->name, node->properties[j].name);
			spa_pod_builder_string(builder, name);
			build_value(builder, &value);
		}
	}
	spa_pod_builder_pop(builder, &values);
	pod = spa_pod_builder_pop(builder, &object);
	if (pod == NULL)
		return -ENOSPC;
	if (props != NULL)
		*props = pod;
	return 1;
}

static int find_qualified_property(struct spa_fgn_graph *graph, const char *name,
		struct fgn_node **node, const struct spa_fgn_property_info **info)
{
	char node_name[256], property_name[256];
	const char *colon = strchr(name, ':');
	size_t len;

	if (colon == NULL || (len = (size_t)(colon - name)) == 0 ||
	    len >= sizeof(node_name) ||
	    copy_string(property_name, sizeof(property_name), colon + 1) < 0)
		return -EINVAL;
	memcpy(node_name, name, len);
	node_name[len] = '\0';
	if ((*node = find_node(graph, node_name)) == NULL ||
	    (*info = find_property(*node, property_name)) == NULL)
		return -ENOENT;
	return 0;
}

static struct fgn_transaction *get_transaction(struct fgn_transaction *transactions,
		uint32_t *n_transactions, struct fgn_node *node)
{
	uint32_t i;

	for (i = 0; i < *n_transactions; i++)
		if (transactions[i].node == node)
			return &transactions[i];
	transactions[*n_transactions].node = node;
	return &transactions[(*n_transactions)++];
}

int spa_fgn_graph_set_props(struct spa_fgn_graph *graph,
		const struct spa_pod *props)
{
	const struct spa_pod_prop *params;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	struct fgn_transaction *transactions;
	uint32_t n_transactions = 0, i;
	int res = 0;

	if (graph == NULL || props == NULL ||
	    !spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props) ||
	    SPA_POD_OBJECT_ID(props) != SPA_PARAM_Props)
		return -EINVAL;
	params = spa_pod_find_prop(props, NULL, SPA_PROP_params);
	if (params == NULL || !spa_pod_is_struct(&params->value))
		return -EINVAL;
	if ((transactions = calloc(graph->n_nodes, sizeof(*transactions))) == NULL)
		return -ENOMEM;

	spa_pod_parser_pod(&parser, &params->value);
	if (spa_pod_parser_push_struct(&parser, &frame) < 0) {
		res = -EINVAL;
		goto done;
	}
	for (;;) {
		const struct spa_fgn_property_info *info;
		struct fgn_transaction *transaction;
		struct spa_fgn_property *new_properties;
		struct spa_fgn_value value;
		struct fgn_node *node;
		struct spa_pod *value_pod;
		const char *name;
		uint32_t j;

		if (spa_pod_parser_get_string(&parser, &name) < 0)
			break;
		if (spa_pod_parser_get_pod(&parser, &value_pod) < 0 ||
		    (res = find_qualified_property(graph, name, &node, &info)) < 0 ||
		    (info->flags & SPA_FGN_PROPERTY_FLAG_READONLY) ||
		    (res = value_from_pod(value_pod, &value)) < 0 ||
		    (res = validate_value(info, &value)) < 0) {
			if (res == 0)
				res = -EPERM;
			goto done;
		}
		transaction = get_transaction(transactions, &n_transactions, node);
		for (j = 0; j < transaction->n_properties; j++)
			if (transaction->properties[j].id == info->id) {
				res = -EEXIST;
				goto done;
			}
		new_properties = realloc(transaction->properties,
				sizeof(*new_properties) * (transaction->n_properties + 1));
		if (new_properties == NULL) {
			res = -ENOMEM;
			goto done;
		}
		transaction->properties = new_properties;
		transaction->properties[transaction->n_properties++] =
				(struct spa_fgn_property) { .id = info->id, .value = value };
	}
	for (i = 0; i < n_transactions; i++) {
		struct fgn_transaction *transaction = &transactions[i];
		const struct spa_fgn_descriptor *descriptor = transaction->node->descriptor;
		if (descriptor->prepare_props == NULL || descriptor->commit_props == NULL) {
			res = -ENOTSUP;
			goto done;
		}
		if ((res = descriptor->prepare_props(transaction->node->instance,
				transaction->properties, transaction->n_properties,
				&transaction->prepared)) < 0)
			goto done;
	}
	for (i = 0; i < n_transactions; i++) {
		struct fgn_transaction *transaction = &transactions[i];
		transaction->node->descriptor->commit_props(transaction->node->instance,
				transaction->prepared);
		transaction->prepared = NULL;
	}
done:
	for (i = 0; i < n_transactions; i++) {
		struct fgn_transaction *transaction = &transactions[i];
		if (transaction->prepared != NULL &&
		    transaction->node->descriptor->discard_props != NULL)
			transaction->node->descriptor->discard_props(
					transaction->node->instance, transaction->prepared);
		free(transaction->properties);
	}
	free(transactions);
	return res;
}

int spa_fgn_graph_update_parameter(struct spa_fgn_graph *graph,
		uint32_t input_port, struct spa_buffer *buffer)
{
	struct spa_fgn_buffer update;
	struct fgn_port *port;
	void *prepared = NULL;
	int res;

	if (graph == NULL || input_port >= graph->n_inputs || buffer == NULL)
		return -EINVAL;
	port = graph->inputs[input_port];
	if (!(port->info->flags & SPA_FGN_PORT_FLAG_PARAMETER))
		return -EINVAL;
	if ((res = validate_buffer(buffer, port->format, false)) < 0)
		return res;
	update = (struct spa_fgn_buffer) {
		.buffer = buffer,
		.format = port->format,
	};
	res = port->node->descriptor->prepare_parameter(port->node->instance,
			port->info->index, &update, &prepared);
	if (res < 0) {
		if (prepared != NULL)
			port->node->descriptor->discard_parameter(
					port->node->instance, prepared);
		return res;
	}
	port->node->descriptor->commit_parameter(port->node->instance, prepared);
	return 0;
}

int spa_fgn_graph_activate(struct spa_fgn_graph *graph)
{
	uint32_t i;
	int res;

	if (graph == NULL)
		return -EINVAL;
	if (graph->active)
		return 0;
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->order[i];
		if (node->descriptor->activate != NULL &&
		    (res = node->descriptor->activate(node->instance)) < 0) {
			while (i > 0) {
				node = graph->order[--i];
				if (node->descriptor->deactivate != NULL)
					node->descriptor->deactivate(node->instance);
			}
			return res;
		}
	}
	graph->active = true;
	return 0;
}

int spa_fgn_graph_deactivate(struct spa_fgn_graph *graph)
{
	int res = 0;
	uint32_t i;

	if (graph == NULL)
		return -EINVAL;
	if (!graph->active)
		return 0;
	for (i = graph->n_nodes; i > 0; i--) {
		struct fgn_node *node = graph->order[i - 1];
		if (node->descriptor->deactivate != NULL) {
			int r = node->descriptor->deactivate(node->instance);
			if (res == 0 && r < 0)
				res = r;
		}
	}
	graph->active = false;
	return res;
}

int spa_fgn_graph_reset(struct spa_fgn_graph *graph)
{
	uint32_t i;

	if (graph == NULL)
		return -EINVAL;
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->order[i];
		int res;
		if (node->descriptor->reset != NULL &&
		    (res = node->descriptor->reset(node->instance)) < 0)
			return res;
	}
	return 0;
}

static int validate_buffer(struct spa_buffer *buffer,
		const struct spa_fgn_format *format, bool output)
{
	struct spa_data *data;
	size_t size;
	int res;

	if (buffer == NULL)
		return -ENOBUFS;
	if (buffer->n_datas == 0 || buffer->datas == NULL)
		return -EINVAL;
	if ((res = format_size(format, &size)) < 0)
		return res;
	data = &buffer->datas[0];
	if (data->data == NULL || data->chunk == NULL || data->maxsize < size)
		return -ENOSPC;
	if (output) {
		data->chunk->offset = 0;
		data->chunk->size = size;
		data->chunk->flags = 0;
	} else if (data->chunk->offset >= data->maxsize ||
		   data->chunk->size < size ||
		   data->maxsize - data->chunk->offset < size) {
		return -EMSGSIZE;
	}
	return 0;
}

int spa_fgn_graph_process(struct spa_fgn_graph *graph,
		struct spa_buffer *const inputs[], uint32_t n_inputs,
		struct spa_buffer *const outputs[], uint32_t n_outputs)
{
	uint32_t i, j;
	int res;

	if (graph == NULL || !graph->active || n_inputs != graph->n_inputs ||
	    n_outputs != graph->n_outputs || inputs == NULL || outputs == NULL)
		return -EINVAL;
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->nodes[i];
		for (j = 0; j < node->n_inputs; j++)
			node->inputs[j]->cycle_buffer = NULL;
		for (j = 0; j < node->n_outputs; j++)
			node->outputs[j]->cycle_buffer = &node->outputs[j]->storage.buffer;
	}
	for (i = 0; i < n_inputs; i++) {
		struct fgn_port *port = graph->inputs[i];
		if (port->info->flags & SPA_FGN_PORT_FLAG_PARAMETER) {
			if (inputs[i] != NULL)
				return -EINVAL;
			continue;
		}
		if (inputs[i] == NULL &&
		    (port->info->flags & SPA_FGN_PORT_FLAG_OPTIONAL))
			continue;
		if ((res = validate_buffer(inputs[i], port->format, false)) < 0)
			return res;
		port->cycle_buffer = inputs[i];
	}
	for (i = 0; i < n_outputs; i++) {
		if ((res = validate_buffer(outputs[i], graph->outputs[i]->format, true)) < 0)
			return res;
		graph->outputs[i]->cycle_buffer = outputs[i];
	}
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->order[i];

		for (j = 0; j < node->n_inputs; j++) {
			struct fgn_port *port = node->inputs[j];
			struct spa_buffer *buffer = port->source != NULL
				? port->source->cycle_buffer : port->cycle_buffer;
			if (buffer == NULL && !(port->info->flags & SPA_FGN_PORT_FLAG_OPTIONAL))
				return -ENOBUFS;
			node->process_inputs[j] = (struct spa_fgn_buffer) {
				.buffer = buffer,
				.format = port->format,
			};
		}
		for (j = 0; j < node->n_outputs; j++) {
			struct fgn_port *port = node->outputs[j];
			node->process_outputs[j] = (struct spa_fgn_buffer) {
				.buffer = port->cycle_buffer,
				.format = port->format,
			};
		}
		if ((res = node->descriptor->process(node->instance,
				node->process_inputs, node->n_inputs,
				node->process_outputs, node->n_outputs)) < 0)
			return res;
	}
	return 0;
}
