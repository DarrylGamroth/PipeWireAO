/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/filter-graph/filter-graph-ndarray.h>
#include <spa/param/ndarray.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/pod/iter.h>
#include <spa/utils/json.h>
#include <spa/utils/json-builder.h>
#include <spa/utils/overflow.h>
#include <spa/utils/string.h>

#define FGN_MAX_NODES 1024u
#define FGN_MAX_PORTS_PER_NODE 1024u
#define FGN_MAX_PROPERTIES_PER_NODE 4096u
#define FGN_MAX_CHOICES_PER_PROPERTY 1024u
#define FGN_MAX_LINKS 4096u
#define FGN_MAX_EXTERNAL_PORTS 1024u
#define FGN_MAX_TRANSACTION_ASSIGNMENTS 4096u

static int next_array_size(uint32_t count, uint32_t maximum,
		size_t element_size, size_t *size)
{
	size_t next;

	if (size == NULL || count >= maximum)
		return -E2BIG;
	next = (size_t)count + 1u;
	if (element_size != 0 && next > SIZE_MAX / element_size)
		return -EOVERFLOW;
	*size = next * element_size;
	return 0;
}

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
	uint32_t cycle_offset;
};

struct fgn_node {
	char *name;
	void *library;
	bool retain_library;
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
	void *initial_prepared;
	bool process_thread_prepared;
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
	struct spa_fgn_worker_group *workers;
	bool active;
	bool process_thread_prepared;
	struct fgn_transaction *property_transactions;
	uint32_t n_property_transactions;
	_Atomic uint32_t property_transaction_state;
	struct fgn_parameter_transaction *parameter_transactions;
	uint32_t n_parameter_transactions;
	_Atomic uint32_t parameter_transaction_state;
};

struct fgn_transaction {
	struct fgn_node *node;
	struct spa_fgn_property *properties;
	uint32_t n_properties;
	void *prepared;
};

struct fgn_parameter_transaction {
	struct fgn_node *node;
	struct spa_fgn_parameter *parameters;
	uint32_t n_parameters;
	void *prepared;
};

enum fgn_transaction_state {
	FGN_TRANSACTION_EMPTY,
	FGN_TRANSACTION_PENDING,
	FGN_TRANSACTION_PROCESSING,
	FGN_TRANSACTION_RETIRED,
};

static int validate_buffer(struct spa_buffer *buffer,
		const struct spa_fgn_format *format, bool output);
static int validate_buffer_nonalias(struct spa_buffer *buffer,
		const struct spa_fgn_format *format);
static void clear_graph_property_transaction(struct spa_fgn_graph *graph);
static void clear_graph_parameter_transaction(struct spa_fgn_graph *graph);

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
	if ((res = posix_memalign(&storage->memory, SPA_CACHE_LINE_SIZE, size)) != 0)
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

static char *canonicalize_json_object(struct spa_json *json, const char *value,
		int value_len)
{
	struct spa_json item = SPA_JSON_START(json, value);
	const char *token;
	char *relaxed, *result;
	int len;

	if (!spa_json_is_object(value, value_len))
		return NULL;
	if ((len = spa_json_next(&item, &token)) <= 0)
		return NULL;
	if (spa_json_is_container(token, len) &&
	    (len = spa_json_container_len(&item, token, len)) <= 0)
		return NULL;
	if ((relaxed = malloc((size_t)len + 1)) == NULL)
		return NULL;
	if (spa_json_parse_stringn(token, len, relaxed, len + 1) <= 0) {
		free(relaxed);
		return NULL;
	}
	result = spa_json_builder_reformat(relaxed, 0);
	free(relaxed);
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
		enum spa_direction direction)
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
		const char *reference, enum spa_direction direction)
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

#define SPA_FGN_PROPERTY_UPDATE_MASK (SPA_FGN_PROPERTY_FLAG_READONLY | \
		SPA_FGN_PROPERTY_FLAG_RUNTIME | \
		SPA_FGN_PROPERTY_FLAG_CONSTRUCTION | \
		SPA_FGN_PROPERTY_FLAG_GRAPH_REBUILD)

static bool value_equal(const struct spa_fgn_value *a,
		const struct spa_fgn_value *b)
{
	if (a->type != b->type)
		return false;
	switch (a->type) {
	case SPA_TYPE_Bool:
		return (a->value.boolean != 0) == (b->value.boolean != 0);
	case SPA_TYPE_Int:
		return a->value.integer == b->value.integer;
	case SPA_TYPE_Long:
		return a->value.long_integer == b->value.long_integer;
	case SPA_TYPE_Float:
		return a->value.float_value == b->value.float_value;
	case SPA_TYPE_Double:
		return a->value.double_value == b->value.double_value;
	case SPA_TYPE_Id:
		return a->value.id == b->value.id;
	case SPA_TYPE_String:
		return string_equal(a->value.string, b->value.string);
	default:
		return false;
	}
}

static int validate_value(const struct spa_fgn_property_info *info,
		const struct spa_fgn_value *value)
{
	uint32_t i;

	if (info == NULL || value == NULL ||
	    value->type != info->default_value.type)
		return -EINVAL;
	if (value->type == SPA_TYPE_String && value->value.string == NULL)
		return -EINVAL;
	if (info->flags & SPA_FGN_PROPERTY_FLAG_CHOICES) {
		for (i = 0; i < info->n_choices; i++)
			if (value_equal(value, &info->choices[i].value))
				return 0;
		return -ERANGE;
	}
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
	uint32_t update_flags, i, j;

	if (info->struct_size < sizeof(*info) || info->name == NULL ||
	    info->name[0] == '\0' ||
	    strlen(info->name) > SPA_FGN_LOCAL_NAME_MAX ||
	    info->description == NULL ||
	    info->description[0] == '\0' || info->unit == NULL ||
	    info->unit[0] == '\0' ||
	    (info->flags & ~(SPA_FGN_PROPERTY_FLAG_READONLY |
			SPA_FGN_PROPERTY_FLAG_RANGE |
			SPA_FGN_PROPERTY_FLAG_RUNTIME |
			SPA_FGN_PROPERTY_FLAG_CONSTRUCTION |
			SPA_FGN_PROPERTY_FLAG_GRAPH_REBUILD |
			SPA_FGN_PROPERTY_FLAG_CHOICES)) != 0 ||
	    !value_type_supported(info->default_value.type))
		return -EINVAL;
	update_flags = info->flags & SPA_FGN_PROPERTY_UPDATE_MASK;
	if (update_flags == 0 || (update_flags & (update_flags - 1)) != 0 ||
	    ((info->flags & SPA_FGN_PROPERTY_FLAG_RANGE) &&
	     (info->flags & SPA_FGN_PROPERTY_FLAG_CHOICES)) ||
	    ((info->n_choices == 0) != (info->choices == NULL)) ||
	    info->n_choices > FGN_MAX_CHOICES_PER_PROPERTY ||
	    (!(info->flags & SPA_FGN_PROPERTY_FLAG_CHOICES) &&
	     info->n_choices != 0))
		return -EINVAL;
	if (info->flags & SPA_FGN_PROPERTY_FLAG_CHOICES) {
		if (info->n_choices == 0)
			return -EINVAL;
		for (i = 0; i < info->n_choices; i++) {
			const struct spa_fgn_property_choice *choice = &info->choices[i];
			if (choice->struct_size < sizeof(*choice) || choice->name == NULL ||
			    choice->name[0] == '\0' ||
			    choice->value.type != info->default_value.type ||
			    (choice->value.type == SPA_TYPE_String &&
			     choice->value.value.string == NULL))
				return -EINVAL;
			for (j = 0; j < i; j++)
				if (spa_streq(choice->name, info->choices[j].name) ||
				    value_equal(&choice->value, &info->choices[j].value))
					return -EEXIST;
		}
		return validate_value(info, &info->default_value);
	}
	if (!(info->flags & SPA_FGN_PROPERTY_FLAG_RANGE))
		return validate_value(info, &info->default_value);
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
		if (spa_json_parse_int(token, len, &integer) <= 0)
			return -EINVAL;
		value->value.integer = integer;
		break;
	case SPA_TYPE_Id: {
		unsigned long long id;
		if (len <= 0 || (size_t)len >= sizeof(number) || token[0] == '-')
			return -EINVAL;
		memcpy(number, token, len);
		number[len] = '\0';
		errno = 0;
		id = strtoull(number, &end, 0);
		if (errno != 0 || end != number + len || id > UINT32_MAX)
			return -EINVAL;
		value->value.id = (uint32_t)id;
		break;
	}
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

static int node_prepare_initial_props(struct fgn_node *node, struct spa_json *json)
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
		size_t properties_size, strings_size;
		uint32_t i;

		if (info == NULL) {
			res = -ENOENT;
			goto done;
		}
		if (!(info->flags & SPA_FGN_PROPERTY_FLAG_RUNTIME)) {
			res = -EPERM;
			goto done;
		}
		for (i = 0; i < n_properties; i++)
			if (properties[i].id == info->id) {
				res = -EEXIST;
				goto done;
			}
		if ((res = next_array_size(n_properties,
				FGN_MAX_PROPERTIES_PER_NODE,
				sizeof(*properties), &properties_size)) < 0 ||
		    (tmp = realloc(properties, properties_size)) == NULL) {
			if (res == 0)
				res = -ENOMEM;
			goto done;
		}
		properties = tmp;
		if ((res = next_array_size(n_properties,
				FGN_MAX_PROPERTIES_PER_NODE,
				sizeof(*owned_strings), &strings_size)) < 0 ||
		    (new_strings = realloc(owned_strings, strings_size)) == NULL) {
			if (res == 0)
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
	if (len < 0) {
		res = -EINVAL;
		goto done;
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
	node->initial_prepared = prepared;
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
	if (node->initial_prepared != NULL && node->instance != NULL &&
	    node->descriptor != NULL && node->descriptor->discard_props != NULL)
		node->descriptor->discard_props(node->instance,
				node->initial_prepared);
	if (node->instance != NULL && node->descriptor != NULL &&
	    node->descriptor->cleanup != NULL)
		node->descriptor->cleanup(node->instance);
	if (node->ports != NULL)
		for (i = 0; i < node->descriptor->n_ports; i++)
			if (node->ports[i].info != NULL &&
			    node->ports[i].info->direction == SPA_DIRECTION_OUTPUT)
				storage_clear(&node->ports[i].storage);
	if (node->library != NULL && !node->retain_library)
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
			if (config != NULL || (config = canonicalize_json_object(json,
					token, len)) == NULL) {
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
	    node->plugin->name == NULL || node->plugin->name[0] == '\0' ||
	    node->plugin->find_descriptor == NULL ||
	    (node->plugin->flags & ~SPA_FGN_PLUGIN_FLAG_RETAIN_LIBRARY) != 0) {
		res = -ENOTSUP;
		goto error;
	}
	node->retain_library =
		(node->plugin->flags & SPA_FGN_PLUGIN_FLAG_RETAIN_LIBRARY) != 0;
	if ((node->descriptor = node->plugin->find_descriptor(label)) == NULL ||
	    node->descriptor->struct_size < SPA_FGN_DESCRIPTOR_ABI_V7_SIZE ||
	    node->descriptor->version != SPA_FGN_PLUGIN_ABI_VERSION ||
	    node->descriptor->name == NULL ||
	    !spa_streq(node->descriptor->name, label) ||
	    strlen(node->descriptor->name) > SPA_FGN_LOCAL_NAME_MAX ||
	    node->descriptor->n_ports > FGN_MAX_PORTS_PER_NODE ||
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
	    ((node->descriptor->prepare_parameters != NULL ||
	      node->descriptor->adopt_parameters != NULL) &&
	     (node->descriptor->prepare_parameters == NULL ||
	      node->descriptor->adopt_parameters == NULL ||
	      node->descriptor->commit_parameter == NULL ||
	      node->descriptor->discard_parameter == NULL)) ||
	    node->descriptor->process == NULL) {
		res = -ENOTSUP;
		goto error;
	}
	if ((res = node->descriptor->instantiate(node->descriptor,
			config ?: "{}",
			spa_fgn_worker_group_get_executor(graph->workers),
			&node->instance)) < 0 || node->instance == NULL)
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
		    strlen(info->name) > SPA_FGN_LOCAL_NAME_MAX ||
		    (info->flags & ~(SPA_FGN_PORT_FLAG_OPTIONAL |
				SPA_FGN_PORT_FLAG_PARAMETER |
				SPA_FGN_PORT_FLAG_CONDITIONAL)) != 0 ||
		    ((info->flags & SPA_FGN_PORT_FLAG_CONDITIONAL) &&
		     info->direction != SPA_DIRECTION_OUTPUT) ||
		    ((info->flags & SPA_FGN_PORT_FLAG_PARAMETER) &&
		     (info->direction != SPA_DIRECTION_INPUT ||
		      !(info->flags & SPA_FGN_PORT_FLAG_OPTIONAL) ||
		      node->descriptor->prepare_parameter == NULL)) ||
		    (info->direction != SPA_DIRECTION_INPUT &&
		     info->direction != SPA_DIRECTION_OUTPUT)) {
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
		if (info->direction == SPA_DIRECTION_INPUT)
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
		if (port->info->direction == SPA_DIRECTION_INPUT)
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
			size_t properties_size;
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
			if ((res = next_array_size(node->n_properties,
					FGN_MAX_PROPERTIES_PER_NODE,
					sizeof(*tmp), &properties_size)) < 0 ||
			    (tmp = realloc(node->properties, properties_size)) == NULL) {
				if (res == 0)
					res = -ENOMEM;
				goto error;
			}
			node->properties = tmp;
			node->properties[node->n_properties++] = info;
		}
	}
	if (have_props &&
	    (res = node_prepare_initial_props(node, &props)) < 0)
		goto error;
	{
		struct fgn_node **tmp;
		size_t nodes_size;
		if ((res = next_array_size(graph->n_nodes, FGN_MAX_NODES,
				sizeof(*tmp), &nodes_size)) < 0)
			goto error;
		tmp = realloc(graph->nodes, nodes_size);
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
	size_t links_size;
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
	if ((out_port = resolve_port(graph, output, SPA_DIRECTION_OUTPUT)) == NULL ||
	    (in_port = resolve_port(graph, input, SPA_DIRECTION_INPUT)) == NULL)
		return -ENOENT;
	if (in_port->info->flags & SPA_FGN_PORT_FLAG_PARAMETER)
		return -ENOTSUP;
	if (in_port->source != NULL || in_port->external != SPA_ID_INVALID)
		return -EBUSY;
	if (!format_equal(out_port->format, in_port->format))
		return -EINVAL;
	if ((len = next_array_size(graph->n_links, FGN_MAX_LINKS,
			 sizeof(*links), &links_size)) < 0)
		return len;
	if ((links = realloc(graph->links, links_size)) == NULL)
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
		enum spa_direction direction)
{
	struct fgn_port ***ports = direction == SPA_DIRECTION_INPUT
		? &graph->inputs : &graph->outputs;
	uint32_t *n_ports = direction == SPA_DIRECTION_INPUT
		? &graph->n_inputs : &graph->n_outputs;
	char reference[512];

	while (spa_json_get_string(json, reference, sizeof(reference)) > 0) {
		struct fgn_port *port = resolve_port(graph, reference, direction);
		struct fgn_port **tmp;
		size_t ports_size;
		int res;

		if (port == NULL || port->external != SPA_ID_INVALID ||
		    (direction == SPA_DIRECTION_INPUT && port->source != NULL))
			return -EINVAL;
		if ((res = next_array_size(*n_ports, FGN_MAX_EXTERNAL_PORTS,
				sizeof(*tmp), &ports_size)) < 0)
			return res;
		if ((tmp = realloc(*ports, ports_size)) == NULL)
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
			size_t ports_size;
			int res;
			if (port->source != NULL)
				continue;
			if ((res = next_array_size(graph->n_inputs,
					FGN_MAX_EXTERNAL_PORTS,
					sizeof(*tmp), &ports_size)) < 0)
				return res;
			if ((tmp = realloc(graph->inputs, ports_size)) == NULL)
				return -ENOMEM;
			graph->inputs = tmp;
			port->external = graph->n_inputs;
			graph->inputs[graph->n_inputs++] = port;
		}
	if (graph->n_outputs == 0)
		for (i = 0; i < last->n_outputs; i++) {
			struct fgn_port *port = last->outputs[i];
			struct fgn_port **tmp;
			size_t ports_size;
			int res;
			if ((res = next_array_size(graph->n_outputs,
					FGN_MAX_EXTERNAL_PORTS,
					sizeof(*tmp), &ports_size)) < 0)
				return res;
			if ((tmp = realloc(graph->outputs, ports_size)) == NULL)
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

static bool format_has_rate(const struct spa_fgn_format *format)
{
	return format->rate_num != 0;
}

static bool format_rate_equal(const struct spa_fgn_format *first,
		const struct spa_fgn_format *second)
{
	return first->rate_num == second->rate_num &&
		first->rate_denom == second->rate_denom;
}

static int validate_graph(const struct spa_fgn_graph *graph)
{
	const struct spa_fgn_format *activation_rate = NULL;
	uint32_t i, j;

	if (graph->n_inputs == 0 && graph->n_outputs == 0)
		return -EINVAL;
	for (i = 0; i < graph->n_inputs; i++) {
		const struct fgn_port *port = graph->inputs[i];

		if (port->info->flags & SPA_FGN_PORT_FLAG_PARAMETER ||
		    !format_has_rate(port->format))
			continue;
		if (activation_rate == NULL)
			activation_rate = port->format;
		else if (!format_rate_equal(activation_rate, port->format))
			return -EINVAL;
	}
	for (i = 0; i < graph->n_nodes; i++) {
		const struct fgn_node *node = graph->nodes[i];
		const struct spa_fgn_format *input_rate = NULL;

		for (j = 0; j < node->n_inputs; j++) {
			const struct fgn_port *port = node->inputs[j];
			if (port->source == NULL && port->external == SPA_ID_INVALID &&
			    !(port->info->flags & SPA_FGN_PORT_FLAG_OPTIONAL))
				return -ENOTCONN;
			if (port->info->flags & SPA_FGN_PORT_FLAG_PARAMETER ||
			    !format_has_rate(port->format))
				continue;
			if (input_rate == NULL)
				input_rate = port->format;
			else if (!format_rate_equal(input_rate, port->format))
				return -EINVAL;
		}
		for (j = 0; j < node->n_outputs; j++) {
			const struct fgn_port *port = node->outputs[j];

			if (input_rate == NULL || !format_has_rate(port->format) ||
			    format_rate_equal(input_rate, port->format))
				continue;
			if (!(port->info->flags & SPA_FGN_PORT_FLAG_CONDITIONAL))
				return -EINVAL;
		}
	}
	return 0;
}

static int queue_initial_property_transaction(struct spa_fgn_graph *graph)
{
	struct fgn_transaction *transactions;
	uint32_t count = 0, i;

	for (i = 0; i < graph->n_nodes; i++)
		if (graph->nodes[i]->initial_prepared != NULL)
			count++;
	if (count == 0)
		return 0;
	if ((transactions = calloc(count, sizeof(*transactions))) == NULL)
		return -ENOMEM;
	count = 0;
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->nodes[i];

		if (node->initial_prepared == NULL)
			continue;
		transactions[count++] = (struct fgn_transaction) {
			.node = node,
			.prepared = node->initial_prepared,
		};
		node->initial_prepared = NULL;
	}
	graph->property_transactions = transactions;
	graph->n_property_transactions = count;
	atomic_store_explicit(&graph->property_transaction_state,
			FGN_TRANSACTION_PENDING, memory_order_release);
	return 0;
}

static int parse_workers(struct spa_json *parent, const char *token, int len,
		uint32_t *n_helpers)
{
	struct spa_json object;
	bool have_helpers = false;
	char key[256];
	int value, item_len;
	const char *item;

	if (n_helpers == NULL || !spa_json_is_object(token, len))
		return -EINVAL;
	spa_json_enter(parent, &object);
	while ((item_len = spa_json_object_next(&object, key, sizeof(key),
			&item)) > 0) {
		if (!spa_streq(key, "helpers") || have_helpers ||
		    spa_json_parse_int(item, item_len, &value) <= 0 || value < 0 ||
		    value > (int)SPA_FGN_EXECUTOR_MAX_HELPERS)
			return -EINVAL;
		*n_helpers = (uint32_t)value;
		have_helpers = true;
	}
	return have_helpers ? 0 : -EINVAL;
}

int spa_fgn_graph_new(const char *config, struct spa_fgn_graph **result)
{
	struct spa_fgn_graph *graph;
	struct spa_json top, child, nodes = { 0 }, links = { 0 };
	struct spa_json inputs = { 0 }, outputs = { 0 };
	bool have_nodes = false, have_links = false;
	bool have_inputs = false, have_outputs = false, have_workers = false;
	uint32_t n_helpers = 0;
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

		if (spa_streq(key, "workers")) {
			if (have_workers ||
			    (res = parse_workers(&top, token, len, &n_helpers)) < 0)
				goto error;
			have_workers = true;
			continue;
		} else if (spa_streq(key, "nodes")) {
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
	if ((res = spa_fgn_worker_group_new(n_helpers, &graph->workers)) < 0)
		goto error;
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
			SPA_DIRECTION_INPUT)) < 0)
		goto error;
	if (have_outputs && (res = load_external_ports(graph, &outputs,
			SPA_DIRECTION_OUTPUT)) < 0)
		goto error;
	if ((!have_inputs || !have_outputs) && (res = load_default_ports(graph)) < 0)
		goto error;
	if ((res = validate_graph(graph)) < 0 || (res = sort_graph(graph)) < 0 ||
	    (res = queue_initial_property_transaction(graph)) < 0)
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
	clear_graph_property_transaction(graph);
	clear_graph_parameter_transaction(graph);
	for (i = graph->n_nodes; i > 0; i--)
		node_free(graph->nodes[i - 1]);
	spa_fgn_worker_group_free(graph->workers);
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
		enum spa_direction direction, uint32_t port,
		const struct spa_fgn_format **format)
{
	if (graph == NULL || format == NULL)
		return -EINVAL;
	if (direction == SPA_DIRECTION_INPUT) {
		if (port >= graph->n_inputs)
			return -ENOENT;
		*format = graph->inputs[port]->format;
	} else if (direction == SPA_DIRECTION_OUTPUT) {
		if (port >= graph->n_outputs)
			return -ENOENT;
		*format = graph->outputs[port]->format;
	} else {
		return -EINVAL;
	}
	return 0;
}

int spa_fgn_graph_get_port_info(const struct spa_fgn_graph *graph,
		enum spa_direction direction, uint32_t port, const char **node_name,
		const struct spa_fgn_port_info **info)
{
	struct fgn_port *graph_port;

	if (graph == NULL || node_name == NULL || info == NULL)
		return -EINVAL;
	if (direction == SPA_DIRECTION_INPUT) {
		if (port >= graph->n_inputs)
			return -ENOENT;
		graph_port = graph->inputs[port];
	} else if (direction == SPA_DIRECTION_OUTPUT) {
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
	flags = !(info->flags & SPA_FGN_PROPERTY_FLAG_RUNTIME)
		? SPA_POD_PROP_FLAG_READONLY : 0;
	spa_pod_builder_prop(builder, SPA_PROP_INFO_type, flags);
	if (info->flags & SPA_FGN_PROPERTY_FLAG_RANGE) {
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Range, 0);
		build_value(builder, &info->default_value);
		build_value(builder, &info->minimum);
		build_value(builder, &info->maximum);
		spa_pod_builder_pop(builder, &choice);
	} else if (info->flags & SPA_FGN_PROPERTY_FLAG_CHOICES) {
		uint32_t i;
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
		build_value(builder, &info->default_value);
		for (i = 0; i < info->n_choices; i++)
			build_value(builder, &info->choices[i].value);
		spa_pod_builder_pop(builder, &choice);
		spa_pod_builder_prop(builder, SPA_PROP_INFO_labels, 0);
		spa_pod_builder_push_struct(builder, &choice);
		for (i = 0; i < info->n_choices; i++) {
			build_value(builder, &info->choices[i].value);
			spa_pod_builder_string(builder,
					info->choices[i].description != NULL &&
					info->choices[i].description[0] != '\0'
					? info->choices[i].description
					: info->choices[i].name);
		}
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
	struct spa_pod_builder_state state;
	struct spa_pod *pod;
	uint32_t i, j;
	int res;

	if (graph == NULL || builder == NULL)
		return -EINVAL;
	spa_pod_builder_get_state(builder, &state);
	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(builder, &values);
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->nodes[i];
		uint64_t revision = node->descriptor->get_prop_revision != NULL
			? node->descriptor->get_prop_revision(node->instance) : 0;
		if ((revision & 1u) != 0) {
			res = -EAGAIN;
			goto error;
		}
		for (j = 0; j < node->n_properties; j++) {
			struct spa_fgn_value value;
			char name[512];
			if (node->descriptor->get_prop == NULL) {
				res = -ENOTSUP;
				goto error;
			}
			if ((res = node->descriptor->get_prop(node->instance,
					node->properties[j].id, &value)) < 0)
				goto error;
			if ((res = validate_value(&node->properties[j], &value)) < 0)
				goto error;
			spa_scnprintf(name, sizeof(name), "%s:%s",
					node->name, node->properties[j].name);
			spa_pod_builder_string(builder, name);
			build_value(builder, &value);
		}
		if (node->descriptor->get_prop_revision != NULL) {
			uint64_t current =
					node->descriptor->get_prop_revision(node->instance);
			if (current != revision || (current & 1u) != 0) {
				res = -EAGAIN;
				goto error;
			}
		}
	}
	spa_pod_builder_pop(builder, &values);
	pod = spa_pod_builder_pop(builder, &object);
	if (pod == NULL) {
		res = -ENOSPC;
		goto error;
	}
	if (props != NULL)
		*props = pod;
	return 1;
error:
	spa_pod_builder_reset(builder, &state);
	return res;
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

static void clear_property_transactions(struct fgn_transaction *transactions,
		uint32_t n_transactions)
{
	uint32_t i;

	for (i = 0; i < n_transactions; i++) {
		struct fgn_transaction *transaction = &transactions[i];
		if (transaction->prepared != NULL &&
		    transaction->node->descriptor->discard_props != NULL)
			transaction->node->descriptor->discard_props(
					transaction->node->instance,
					transaction->prepared);
		free(transaction->properties);
	}
	free(transactions);
}

static void clear_graph_property_transaction(struct spa_fgn_graph *graph)
{
	clear_property_transactions(graph->property_transactions,
			graph->n_property_transactions);
	graph->property_transactions = NULL;
	graph->n_property_transactions = 0;
	atomic_store_explicit(&graph->property_transaction_state,
			FGN_TRANSACTION_EMPTY, memory_order_release);
}

static void reclaim_graph_property_transaction(struct spa_fgn_graph *graph)
{
	if (atomic_load_explicit(&graph->property_transaction_state,
			memory_order_acquire) == FGN_TRANSACTION_RETIRED)
		clear_graph_property_transaction(graph);
}

int spa_fgn_graph_set_props(struct spa_fgn_graph *graph,
		const struct spa_pod *props)
{
	const struct spa_pod_prop *params;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	struct fgn_transaction *transactions;
	uint32_t n_transactions = 0, i;
	int parse_res, res = 0;

	if (graph == NULL || props == NULL ||
	    !spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props) ||
	    SPA_POD_OBJECT_ID(props) != SPA_PARAM_Props)
		return -EINVAL;
	reclaim_graph_property_transaction(graph);
	if (atomic_load_explicit(&graph->property_transaction_state,
			memory_order_acquire) != FGN_TRANSACTION_EMPTY)
		return -EBUSY;
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
		size_t properties_size;

		parse_res = spa_pod_parser_get_string(&parser, &name);
		if (parse_res < 0) {
			if (parser.state.offset ==
			    frame.offset + SPA_POD_SIZE(&frame.pod))
				break;
			res = -EINVAL;
			goto done;
		}
		if (spa_pod_parser_get_pod(&parser, &value_pod) < 0) {
			res = -EINVAL;
			goto done;
		}
		if ((res = find_qualified_property(graph, name, &node, &info)) < 0 ||
		    !(info->flags & SPA_FGN_PROPERTY_FLAG_RUNTIME) ||
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
		if ((res = next_array_size(transaction->n_properties,
				FGN_MAX_TRANSACTION_ASSIGNMENTS,
				sizeof(*new_properties), &properties_size)) < 0)
			goto done;
		new_properties = realloc(transaction->properties, properties_size);
		if (new_properties == NULL) {
			res = -ENOMEM;
			goto done;
		}
		transaction->properties = new_properties;
		transaction->properties[transaction->n_properties++] =
				(struct spa_fgn_property) { .id = info->id, .value = value };
	}
	if (n_transactions == 0)
		goto done;
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
		free(transaction->properties);
		transaction->properties = NULL;
	}
	graph->property_transactions = transactions;
	graph->n_property_transactions = n_transactions;
	transactions = NULL;
	n_transactions = 0;
	atomic_store_explicit(&graph->property_transaction_state,
			FGN_TRANSACTION_PENDING, memory_order_release);
done:
	clear_property_transactions(transactions, n_transactions);
	return res;
}

static struct fgn_parameter_transaction *get_parameter_transaction(
		struct fgn_parameter_transaction *transactions,
		uint32_t *n_transactions, struct fgn_node *node)
{
	uint32_t i;

	for (i = 0; i < *n_transactions; i++)
		if (transactions[i].node == node)
			return &transactions[i];
	transactions[*n_transactions].node = node;
	return &transactions[(*n_transactions)++];
}

static void clear_parameter_transactions(
		struct fgn_parameter_transaction *transactions,
		uint32_t n_transactions)
{
	uint32_t i;

	for (i = 0; i < n_transactions; i++) {
		struct fgn_parameter_transaction *transaction = &transactions[i];
		if (transaction->prepared != NULL &&
		    transaction->node->descriptor->discard_parameter != NULL)
			transaction->node->descriptor->discard_parameter(
					transaction->node->instance,
					transaction->prepared);
		free(transaction->parameters);
	}
	free(transactions);
}

static void clear_graph_parameter_transaction(struct spa_fgn_graph *graph)
{
	clear_parameter_transactions(graph->parameter_transactions,
			graph->n_parameter_transactions);
	graph->parameter_transactions = NULL;
	graph->n_parameter_transactions = 0;
	atomic_store_explicit(&graph->parameter_transaction_state,
			FGN_TRANSACTION_EMPTY, memory_order_release);
}

static void reclaim_graph_parameter_transaction(struct spa_fgn_graph *graph)
{
	if (atomic_load_explicit(&graph->parameter_transaction_state,
			memory_order_acquire) == FGN_TRANSACTION_RETIRED)
		clear_graph_parameter_transaction(graph);
}

int spa_fgn_graph_set_parameters(struct spa_fgn_graph *graph,
		const struct spa_fgn_parameter_update *updates,
		uint32_t n_updates)
{
	struct fgn_parameter_transaction *transactions = NULL;
	uint32_t n_transactions = 0, i;
	int res = 0;

	if (graph == NULL || updates == NULL || n_updates == 0)
		return -EINVAL;
	if (n_updates > SPA_FGN_MAX_PARAMETER_TRANSACTION_ASSIGNMENTS)
		return -E2BIG;
	reclaim_graph_parameter_transaction(graph);
	if (atomic_load_explicit(&graph->parameter_transaction_state,
			memory_order_acquire) != FGN_TRANSACTION_EMPTY)
		return -EBUSY;
	if ((transactions = calloc(n_updates, sizeof(*transactions))) == NULL)
		return -ENOMEM;

	for (i = 0; i < n_updates; i++) {
		const struct spa_fgn_parameter_update *source = &updates[i];
		struct fgn_parameter_transaction *transaction;
		struct spa_fgn_parameter *parameters;
		struct fgn_port *port;
		uint32_t j;
		size_t parameters_size;

		if (source->reserved != 0 || source->input_port >= graph->n_inputs ||
		    source->buffer == NULL) {
			res = -EINVAL;
			goto done;
		}
		for (j = 0; j < i; j++)
			if (updates[j].input_port == source->input_port) {
				res = -EEXIST;
				goto done;
			}
		port = graph->inputs[source->input_port];
		if (!(port->info->flags & SPA_FGN_PORT_FLAG_PARAMETER)) {
			res = -EINVAL;
			goto done;
		}
		if ((res = validate_buffer(source->buffer, port->format, false)) < 0 ||
		    (res = validate_buffer_nonalias(source->buffer, port->format)) < 0)
			goto done;

		transaction = get_parameter_transaction(transactions,
				&n_transactions, port->node);
		if ((res = next_array_size(transaction->n_parameters,
				SPA_FGN_MAX_PARAMETER_TRANSACTION_ASSIGNMENTS,
				sizeof(*parameters), &parameters_size)) < 0)
			goto done;
		parameters = realloc(transaction->parameters, parameters_size);
		if (parameters == NULL) {
			res = -ENOMEM;
			goto done;
		}
		transaction->parameters = parameters;
		transaction->parameters[transaction->n_parameters++] =
				(struct spa_fgn_parameter) {
					.port = port->info->index,
					.buffer = {
						.buffer = source->buffer,
						.format = port->format,
					},
				};
	}

	for (i = 0; i < n_transactions; i++) {
		struct fgn_parameter_transaction *transaction = &transactions[i];
		const struct spa_fgn_descriptor *descriptor = transaction->node->descriptor;

		if (descriptor->prepare_parameters == NULL ||
		    descriptor->adopt_parameters == NULL ||
		    descriptor->commit_parameter == NULL ||
		    descriptor->discard_parameter == NULL) {
			res = -ENOTSUP;
			goto done;
		}
		res = descriptor->prepare_parameters(transaction->node->instance,
				transaction->parameters, transaction->n_parameters,
				&transaction->prepared);
		if (res < 0)
			goto done;
		if (transaction->prepared == NULL) {
			res = -EFAULT;
			goto done;
		}
		free(transaction->parameters);
		transaction->parameters = NULL;
	}

	graph->parameter_transactions = transactions;
	graph->n_parameter_transactions = n_transactions;
	transactions = NULL;
	n_transactions = 0;
	atomic_store_explicit(&graph->parameter_transaction_state,
			FGN_TRANSACTION_PENDING, memory_order_release);
done:
	clear_parameter_transactions(transactions, n_transactions);
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
	if (port->node->descriptor->prepare_parameters != NULL &&
	    port->node->descriptor->adopt_parameters != NULL) {
		const struct spa_fgn_parameter_update transaction = {
			.input_port = input_port,
			.buffer = buffer,
		};
		return spa_fgn_graph_set_parameters(graph, &transaction, 1);
	}
	reclaim_graph_parameter_transaction(graph);
	if (atomic_load_explicit(&graph->parameter_transaction_state,
			memory_order_acquire) != FGN_TRANSACTION_EMPTY)
		return -EBUSY;
	if ((res = validate_buffer(buffer, port->format, false)) < 0)
		return res;
	if ((res = validate_buffer_nonalias(buffer, port->format)) < 0)
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
	if ((res = spa_fgn_worker_group_activate(graph->workers)) < 0)
		return res;
	graph->process_thread_prepared = false;
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->order[i];
		node->process_thread_prepared =
			!spa_fgn_descriptor_has_prepare_process_thread(node->descriptor) ||
			node->descriptor->prepare_process_thread == NULL;
		if (node->descriptor->activate != NULL &&
		    (res = node->descriptor->activate(node->instance)) < 0) {
			while (i > 0) {
				node = graph->order[--i];
				if (node->descriptor->deactivate != NULL)
					node->descriptor->deactivate(node->instance);
			}
			spa_fgn_worker_group_deactivate(graph->workers);
			return res;
		}
	}
	graph->process_thread_prepared = true;
	for (i = 0; i < graph->n_nodes; i++)
		if (!graph->nodes[i]->process_thread_prepared) {
			graph->process_thread_prepared = false;
			break;
		}
	graph->active = true;
	return 0;
}

int spa_fgn_graph_prepare_process_thread(struct spa_fgn_graph *graph)
{
	uint32_t i;

	if (graph == NULL || !graph->active)
		return -EINVAL;
	if (graph->process_thread_prepared)
		return 0;
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->order[i];
		int res;

		if (node->process_thread_prepared)
			continue;
		if (!spa_fgn_descriptor_has_prepare_process_thread(node->descriptor) ||
		    node->descriptor->prepare_process_thread == NULL)
			return -EFAULT;
		if ((res = node->descriptor->prepare_process_thread(
				node->instance)) < 0)
			return res;
		node->process_thread_prepared = true;
	}
	graph->process_thread_prepared = true;
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
	graph->process_thread_prepared = false;
	for (i = 0; i < graph->n_nodes; i++)
		graph->nodes[i]->process_thread_prepared = false;
	{
		int r = spa_fgn_worker_group_deactivate(graph->workers);
		if (res == 0 && r < 0)
			res = r;
	}
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
	size_t size, metadata_bytes = 0;
	uintptr_t payload;
	uint32_t element_size, i, j;
	int res;

	if (buffer == NULL)
		return -ENOBUFS;
	if (buffer->n_datas != 1 || buffer->datas == NULL ||
	    (uintptr_t)buffer->datas % _Alignof(struct spa_data) != 0 ||
	    buffer->n_metas > SPA_FGN_MAX_METAS ||
	    (buffer->n_metas != 0 &&
	     (buffer->metas == NULL ||
	      (uintptr_t)buffer->metas % _Alignof(struct spa_meta) != 0)))
		return -EINVAL;
	if ((res = format_size(format, &size)) < 0)
		return res;
	element_size = spa_element_type_size(format->element_type);
	data = &buffer->datas[0];
	if (data->data == NULL || data->chunk == NULL ||
	    (uintptr_t)data->chunk % _Alignof(struct spa_chunk) != 0)
		return -EINVAL;
	if (data->chunk->offset >= data->maxsize ||
	    data->maxsize - data->chunk->offset < size)
		return -ENOSPC;
	if ((uintptr_t)data->data > UINTPTR_MAX - data->chunk->offset)
		return -EOVERFLOW;
	payload = (uintptr_t)data->data + data->chunk->offset;
	if (payload % element_size != 0)
		return -EINVAL;
	if (!output && (data->chunk->size < size ||
		data->chunk->size > data->maxsize - data->chunk->offset))
		return -EMSGSIZE;
	for (i = 0; i < buffer->n_metas; i++) {
		const struct spa_meta *meta = &buffer->metas[i];
		if ((meta->size != 0 && meta->data == NULL) ||
		    meta->size > SPA_FGN_MAX_META_BYTES ||
		    metadata_bytes > SPA_FGN_MAX_META_BYTES - meta->size)
			return -EINVAL;
		metadata_bytes += meta->size;
		for (j = 0; j < i; j++)
			if (buffer->metas[j].type == meta->type)
				return -EEXIST;
		if (meta->type == SPA_META_Header &&
		    (meta->size < sizeof(struct spa_meta_header) ||
		     (uintptr_t)meta->data % _Alignof(struct spa_meta_header) != 0))
			return -EINVAL;
		if (meta->type == SPA_META_Acquisition &&
		    (meta->size < sizeof(struct spa_meta_acquisition) ||
		     (uintptr_t)meta->data % _Alignof(struct spa_meta_acquisition) != 0))
			return -EINVAL;
	}
	return 0;
}

struct memory_region {
	uintptr_t start;
	uintptr_t end;
};

#define MAX_BUFFER_REGIONS (5u + SPA_FGN_MAX_METAS)

static int memory_region_init(struct memory_region *region,
		const void *pointer, size_t size)
{
	uintptr_t start = (uintptr_t)pointer;

	if (size == 0) {
		region->start = region->end = 0;
		return 0;
	}
	if (pointer == NULL || start > UINTPTR_MAX - size)
		return -EOVERFLOW;
	region->start = start;
	region->end = start + size;
	return 0;
}

static bool memory_regions_overlap(const struct memory_region *first,
		const struct memory_region *second)
{
	return first->start != first->end && second->start != second->end &&
		first->start < second->end && second->start < first->end;
}

static int collect_buffer_regions(struct spa_buffer *buffer,
		const struct spa_fgn_format *format,
		struct memory_region regions[MAX_BUFFER_REGIONS], uint32_t *n_regions)
{
	struct spa_data *data = &buffer->datas[0];
	size_t payload_size;
	uint32_t count = 0, i;
	int res;

#define ADD_REGION(pointer, size) \
	do { \
		if ((res = memory_region_init(&regions[count], (pointer), (size))) < 0) \
			return res; \
		count++; \
	} while (0)

	if ((res = format_size(format, &payload_size)) < 0)
		return res;
	ADD_REGION(buffer, sizeof(*buffer));
	ADD_REGION(buffer->datas, sizeof(*buffer->datas));
	ADD_REGION(data->chunk, sizeof(*data->chunk));
	ADD_REGION(buffer->metas, buffer->n_metas * sizeof(*buffer->metas));
	ADD_REGION((void *)((uintptr_t)data->data + data->chunk->offset),
			payload_size);
	for (i = 0; i < buffer->n_metas; i++)
		ADD_REGION(buffer->metas[i].data, buffer->metas[i].size);
	*n_regions = count;
	return 0;
#undef ADD_REGION
}

static int validate_buffer_nonalias(struct spa_buffer *buffer,
		const struct spa_fgn_format *format)
{
	struct memory_region regions[MAX_BUFFER_REGIONS];
	uint32_t n_regions, i, j;
	int res;

	if ((res = collect_buffer_regions(buffer, format, regions, &n_regions)) < 0)
		return res;
	for (i = 0; i < n_regions; i++)
		for (j = 0; j < i; j++)
			if (memory_regions_overlap(&regions[i], &regions[j]))
				return -EINVAL;
	return 0;
}

static int buffers_overlap(struct spa_buffer *first,
		const struct spa_fgn_format *first_format,
		struct spa_buffer *second,
		const struct spa_fgn_format *second_format, bool *overlap)
{
	struct memory_region first_regions[MAX_BUFFER_REGIONS];
	struct memory_region second_regions[MAX_BUFFER_REGIONS];
	uint32_t n_first, n_second, i, j;
	int res;

	if ((res = collect_buffer_regions(first, first_format,
			first_regions, &n_first)) < 0 ||
	    (res = collect_buffer_regions(second, second_format,
			second_regions, &n_second)) < 0)
		return res;
	*overlap = false;
	for (i = 0; i < n_first; i++)
		for (j = 0; j < n_second; j++)
			if (memory_regions_overlap(&first_regions[i],
					&second_regions[j])) {
				*overlap = true;
				return 0;
			}
	return 0;
}

static int buffer_overlaps_region(struct spa_buffer *buffer,
		const struct spa_fgn_format *format, const void *pointer, size_t size,
		bool *overlap)
{
	struct memory_region buffer_regions[MAX_BUFFER_REGIONS];
	struct memory_region region;
	uint32_t n_buffer_regions, i;
	int res;

	if ((res = collect_buffer_regions(buffer, format, buffer_regions,
			&n_buffer_regions)) < 0 ||
	    (res = memory_region_init(&region, pointer, size)) < 0)
		return res;
	*overlap = false;
	for (i = 0; i < n_buffer_regions; i++)
		if (memory_regions_overlap(&buffer_regions[i], &region)) {
			*overlap = true;
			break;
		}
	return 0;
}

static int validate_output_ownership(struct spa_fgn_graph *graph,
		struct spa_buffer *const inputs[], uint32_t n_inputs,
		struct spa_buffer *const outputs[], uint32_t n_outputs)
{
	uint32_t i, j, k;
	bool overlap;
	int res;

	for (i = 0; i < n_outputs; i++) {
		if ((res = validate_buffer_nonalias(outputs[i],
				graph->outputs[i]->format)) < 0)
			return res;
		if ((res = buffer_overlaps_region(outputs[i],
				graph->outputs[i]->format, inputs,
				n_inputs * sizeof(*inputs), &overlap)) < 0)
			return res;
		if (overlap)
			return -EINVAL;
		if ((res = buffer_overlaps_region(outputs[i],
				graph->outputs[i]->format, outputs,
				n_outputs * sizeof(*outputs), &overlap)) < 0)
			return res;
		if (overlap)
			return -EINVAL;
		for (k = 0; k < graph->n_nodes; k++) {
			const struct fgn_node *node = graph->nodes[k];

			if ((res = buffer_overlaps_region(outputs[i],
					graph->outputs[i]->format,
					node->process_inputs,
					node->n_inputs * sizeof(*node->process_inputs),
					&overlap)) < 0)
				return res;
			if (overlap)
				return -EINVAL;
			if ((res = buffer_overlaps_region(outputs[i],
					graph->outputs[i]->format,
					node->process_outputs,
					node->n_outputs * sizeof(*node->process_outputs),
					&overlap)) < 0)
				return res;
			if (overlap)
				return -EINVAL;
		}
		for (j = 0; j < n_inputs; j++) {
			if (inputs[j] == NULL)
				continue;
			if ((res = validate_buffer_nonalias(inputs[j],
					graph->inputs[j]->format)) < 0)
				return res;
			if ((res = buffers_overlap(outputs[i], graph->outputs[i]->format,
					inputs[j], graph->inputs[j]->format, &overlap)) < 0)
				return res;
			if (overlap)
				return -EINVAL;
		}
		for (j = 0; j < i; j++) {
			if ((res = buffers_overlap(outputs[i], graph->outputs[i]->format,
					outputs[j], graph->outputs[j]->format, &overlap)) < 0)
				return res;
			if (overlap)
				return -EINVAL;
		}
	}
	return 0;
}

static bool publish_graph_property_transaction(struct spa_fgn_graph *graph)
{
	uint32_t expected = FGN_TRANSACTION_PENDING;
	uint32_t i;

	if (!atomic_compare_exchange_strong_explicit(
			&graph->property_transaction_state, &expected,
			FGN_TRANSACTION_PROCESSING,
			memory_order_acquire, memory_order_relaxed))
		return false;
	for (i = 0; i < graph->n_property_transactions; i++) {
		struct fgn_transaction *transaction =
				&graph->property_transactions[i];
		transaction->node->descriptor->commit_props(
				transaction->node->instance,
				transaction->prepared);
		transaction->prepared = NULL;
	}
	atomic_store_explicit(&graph->property_transaction_state,
			FGN_TRANSACTION_RETIRED, memory_order_release);
	return true;
}

static bool publish_graph_parameter_transaction(struct spa_fgn_graph *graph)
{
	uint32_t expected = FGN_TRANSACTION_PENDING;
	uint32_t i;

	if (!atomic_compare_exchange_strong_explicit(
			&graph->parameter_transaction_state, &expected,
			FGN_TRANSACTION_PROCESSING,
			memory_order_acquire, memory_order_relaxed))
		return false;
	for (i = 0; i < graph->n_parameter_transactions; i++) {
		struct fgn_parameter_transaction *transaction =
				&graph->parameter_transactions[i];
		transaction->node->descriptor->commit_parameter(
				transaction->node->instance,
				transaction->prepared);
		transaction->prepared = NULL;
	}
	for (i = 0; i < graph->n_parameter_transactions; i++) {
		struct fgn_parameter_transaction *transaction =
				&graph->parameter_transactions[i];
		transaction->node->descriptor->adopt_parameters(
				transaction->node->instance);
	}
	atomic_store_explicit(&graph->parameter_transaction_state,
			FGN_TRANSACTION_RETIRED, memory_order_release);
	return true;
}

static void clear_external_output_sizes(struct spa_buffer *const outputs[],
		uint32_t n_outputs)
{
	uint32_t i;

	for (i = 0; i < n_outputs; i++)
		outputs[i]->datas[0].chunk->size = 0;
}

static int validate_completed_output(struct spa_buffer *buffer,
		const struct spa_fgn_format *format, uint32_t expected_offset,
		bool conditional)
{
	struct spa_data *data = &buffer->datas[0];
	size_t size;
	int res;

	if ((res = format_size(format, &size)) < 0)
		return res;
	if (conditional && data->chunk->size == 0)
		return data->chunk->offset == expected_offset ? 0 : -EMSGSIZE;
	if (data->chunk->offset != expected_offset || data->chunk->size != size ||
	    data->chunk->offset >= data->maxsize ||
	    data->maxsize - data->chunk->offset < size)
		return -EMSGSIZE;
	return 0;
}

int spa_fgn_graph_process(struct spa_fgn_graph *graph,
		struct spa_buffer *const inputs[], uint32_t n_inputs,
		struct spa_buffer *const outputs[], uint32_t n_outputs)
{
	uint32_t i, j;
	int res, status = SPA_FGN_PROCESS_RESULT_NONE;

	if (graph == NULL || !graph->active || n_inputs != graph->n_inputs ||
	    n_outputs != graph->n_outputs ||
	    (n_inputs != 0 && inputs == NULL) ||
	    (n_outputs != 0 && outputs == NULL))
		return -EINVAL;
	if (!graph->process_thread_prepared)
		return -EAGAIN;
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->nodes[i];
		for (j = 0; j < node->n_inputs; j++)
			node->inputs[j]->cycle_buffer = NULL;
		for (j = 0; j < node->n_outputs; j++) {
			node->outputs[j]->storage.chunk.offset = 0;
			node->outputs[j]->storage.chunk.size = 0;
			node->outputs[j]->cycle_buffer = &node->outputs[j]->storage.buffer;
			node->outputs[j]->cycle_offset = 0;
		}
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
		if ((res = validate_buffer_nonalias(inputs[i], port->format)) < 0)
			return res;
		port->cycle_buffer = inputs[i];
	}
	for (i = 0; i < n_outputs; i++) {
		if ((res = validate_buffer(outputs[i], graph->outputs[i]->format, true)) < 0)
			return res;
	}
	if ((res = validate_output_ownership(graph, inputs, n_inputs,
			outputs, n_outputs)) < 0)
		return res;
	for (i = 0; i < n_outputs; i++) {
		struct spa_data *data = &outputs[i]->datas[0];
		size_t size;

		if ((res = format_size(graph->outputs[i]->format, &size)) < 0)
			return res;
		if (size > UINT32_MAX)
			return -EOVERFLOW;
		data->chunk->size = 0;
		data->chunk->flags = 0;
		graph->outputs[i]->cycle_buffer = outputs[i];
		graph->outputs[i]->cycle_offset = data->chunk->offset;
	}
	if (publish_graph_property_transaction(graph))
		status |= SPA_FGN_PROCESS_RESULT_PROPS_CHANGED;
	if (publish_graph_parameter_transaction(graph))
		status |= SPA_FGN_PROCESS_RESULT_PROPS_CHANGED;
	for (i = 0; i < graph->n_nodes; i++) {
		struct fgn_node *node = graph->order[i];
		uint64_t property_revision = node->descriptor->get_prop_revision != NULL
			? node->descriptor->get_prop_revision(node->instance) : 0;
		bool ready = true;

		for (j = 0; j < node->n_inputs; j++) {
			struct fgn_port *port = node->inputs[j];
			struct spa_buffer *buffer = port->source != NULL
				? port->source->cycle_buffer : port->cycle_buffer;
			if (port->source != NULL && buffer != NULL &&
			    buffer->datas[0].chunk->size == 0)
				buffer = NULL;
			if (buffer == NULL && !(port->info->flags & SPA_FGN_PORT_FLAG_OPTIONAL))
				ready = false;
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
		if (!ready)
			continue;
		if ((res = node->descriptor->process(node->instance,
				node->process_inputs, node->n_inputs,
				node->process_outputs, node->n_outputs)) < 0)
			goto process_error;
		for (j = 0; j < node->n_outputs; j++) {
			struct fgn_port *port = node->outputs[j];
			if ((res = validate_completed_output(port->cycle_buffer,
					port->format, port->cycle_offset,
					port->info->flags &
						SPA_FGN_PORT_FLAG_CONDITIONAL)) < 0)
				goto process_error;
		}
		if (node->descriptor->get_prop_revision != NULL) {
			uint64_t current =
					node->descriptor->get_prop_revision(node->instance);
			if ((property_revision & 1u) != 0 ||
			    current != property_revision || (current & 1u) != 0)
				status |= SPA_FGN_PROCESS_RESULT_PROPS_CHANGED;
		}
	}
	return status;

process_error:
	clear_external_output_sizes(outputs, n_outputs);
	return res;
}
