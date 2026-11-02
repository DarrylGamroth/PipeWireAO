/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "../pipewire/ndarray-filter.c"

static int no_process(void *data SPA_UNUSED,
		const struct pw_ndarray_filter_buffer *inputs SPA_UNUSED, uint32_t n_inputs SPA_UNUSED,
		struct pw_ndarray_filter_buffer *outputs SPA_UNUSED, uint32_t n_outputs SPA_UNUSED)
{
	return 0;
}

static void test_configuration(void)
{
	uint32_t shape = 1;
	struct pw_ndarray_filter_events events = { .version = PW_VERSION_NDARRAY_FILTER_EVENTS,
		.process = no_process };
	struct pw_ndarray_filter_port port = { .struct_size = sizeof(port),
		.direction = SPA_DIRECTION_INPUT, .name = "input", .format = {
			.element_type = SPA_ELEMENT_TYPE_F32_LE, .layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
			.n_dimensions = 1, .shape = &shape } };
	struct pw_ndarray_filter_config config = { .struct_size = sizeof(config),
		.node_name = "buffer-property-configuration", .n_ports = 1,
		.ports = &port, .events = &events };
	struct spa_meta_property_requirement required = { 1, SPA_TYPE_Id };
	struct pw_ndarray_filter *filter;

	spa_assert_se(pw_ndarray_filter_new(&config, &filter) == 0);
	spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_INPUT,
			0, 128, &required, 1) == 0);
	spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_INPUT,
			0, 16, &required, 1) == -ENOSPC);
	{
		struct spa_meta_property_requirement sequence = { SPA_META_PROPERTY_acquisitionSequence, SPA_TYPE_Bytes };
		spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_INPUT,
			0, 40, &sequence, 1) == -ENOSPC);
		spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_INPUT,
			0, 96, &sequence, 1) == 0);
		spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_INPUT,
			0, 128, &required, 1) == 0);
	}
	required.key = 7;
	spa_assert_se(filter->inputs[0]->property_requirements[0].key == 1);
	spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_INPUT,
			0, 15, NULL, 0) == -EINVAL);
	spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_INPUT,
			0, 4096, NULL, 0) == -EINVAL);
	spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_OUTPUT,
			0, 128, NULL, 0) == -EINVAL);
	spa_assert_se(pw_ndarray_filter_set_port_buffer_properties(filter, SPA_DIRECTION_INPUT,
			0, 0, NULL, 0) == 0);
	spa_assert_se(filter->inputs[0]->properties_capacity == 0);
	pw_ndarray_filter_destroy(filter);
}

int main(void)
{
	uint64_t property_storage[64];
	float payload = 1;
	struct spa_chunk chunk = { .size = sizeof(payload), .stride = sizeof(payload) };
	struct spa_data block = { .type = SPA_DATA_MemPtr, .data = &payload,
		.maxsize = sizeof(payload), .chunk = &chunk };
	struct spa_meta_header header = { .seq = 77 };
	struct spa_meta metas[] = {
		{ SPA_META_Header, sizeof(header), &header },
		{ SPA_META_Properties, sizeof(property_storage), property_storage },
		{ SPA_META_TYPE_FEATURES(SPA_META_Properties, SPA_META_FEATURE_PROPERTIES_VERSION_1), 0, NULL },
	};
	struct spa_buffer buffer = { .n_metas = SPA_N_ELEMENTS(metas), .metas = metas,
		.n_datas = 1, .datas = &block };
	struct pw_buffer native = { .buffer = &buffer };
	struct pw_ndarray_filter_buffer view;
	const uint8_t domain[16] = {1};
	struct spa_acquisition_identity identity;
	struct spa_meta_property_requirement requirement = {1, SPA_TYPE_Id};
	struct ndarray_port port = { .size = sizeof(payload), .stride = sizeof(payload),
		.properties_capacity = sizeof(property_storage),
		.n_property_requirements = 1, .property_requirements = &requirement };
	struct ndarray_port *ports[] = {&port};
	struct pw_buffer *native_buffers[] = {&native};
	bool available[] = {true};
	struct pw_ndarray_filter filter = { .n_inputs = 1, .n_outputs = 1,
		.inputs = ports, .outputs = ports, .input_buffers = native_buffers,
		.output_buffers = native_buffers, .input_available = available };
	struct spa_meta borrowed;
	uint32_t optical = SPA_META_OPTICAL_FRINGED, trigger = SPA_META_TRIGGER_LOW;
	int32_t optional = 17;

	spa_assert_se(spa_meta_properties_init(&metas[1]));
	spa_assert_se(spa_meta_properties_add(&metas[1], 1, 0, SPA_TYPE_Id, 4, &optical));
	spa_assert_se(spa_meta_properties_add(&metas[1], 2, 0, SPA_TYPE_Id, 4, &trigger));
	spa_assert_se(spa_meta_properties_add(&metas[1], 3, SPA_POD_PROP_FLAG_MANDATORY, SPA_TYPE_Bytes, 16, domain));
	spa_assert_se(spa_meta_properties_add_uint64(&metas[1], 4, SPA_POD_PROP_FLAG_MANDATORY, UINT64_MAX));
	spa_assert_se(spa_meta_properties_add_uint64(&metas[1], 5, SPA_POD_PROP_FLAG_MANDATORY, 9));
	spa_assert_se(spa_meta_properties_add(&metas[1], 19, 0, SPA_TYPE_Int, 4, &optional));
	spa_assert_se(project_buffer(&native, &port, false, &view) == 0);
	spa_assert_se(view.metadata_valid & PW_NDARRAY_FILTER_METADATA_PROPERTIES);
	spa_assert_se(view.properties.data == property_storage);
	spa_assert_se(spa_meta_properties_get_acquisition_identity(&view.properties, &identity));
	spa_assert_se(identity.sequence == 9 && identity.generation == UINT64_MAX);
	spa_assert_se(view.struct_size == sizeof(struct pw_ndarray_filter_buffer));
	spa_assert_se(pw_ndarray_filter_get_buffer_properties(&filter, SPA_DIRECTION_INPUT, 0, &borrowed) == -EPERM);
	filter.in_process = true;
	spa_assert_se(pw_ndarray_filter_get_buffer_properties(&filter, SPA_DIRECTION_INPUT, 0, &borrowed) == 0);
	spa_assert_se(borrowed.data == property_storage);
	spa_assert_se(spa_meta_properties_find(&borrowed, 19) != NULL);
	available[0] = false;
	spa_assert_se(pw_ndarray_filter_get_buffer_properties(&filter, SPA_DIRECTION_INPUT, 0, &borrowed) == -ENOENT);
	available[0] = true;
	filter.in_process = false;
	header.seq++;
	spa_assert_se(project_buffer(&native, &port, false, &view) == 0);
	header.seq--;
	buffer.n_metas = 2;
	spa_assert_se(project_buffer(&native, &port, false, &view) == -ENOTSUP);
	buffer.n_metas = 3;
	spa_assert_se(project_buffer(&native, &port, true, &view) == 0);
	spa_assert_se(initialize_output_properties(&port, &native) == 0);
	spa_assert_se(spa_meta_properties_find(&metas[1], 1) == NULL);
	spa_assert_se(spa_meta_properties_find(&metas[1], 19) == NULL);
	spa_assert_se(validate_completed_output(&port, &native, &view) == -EINVAL);
	view.flags = PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE;
	spa_assert_se(validate_completed_output(&port, &native, &view) == 0);
	view.flags = 0;
	spa_assert_se(spa_meta_properties_add(&metas[1], 1, 0, SPA_TYPE_Id, 4, &optical));
	spa_assert_se(spa_meta_properties_add(&metas[1], SPA_META_PROPERTY_triggerClassification, 0, SPA_TYPE_Id, 4, &trigger));
	view.header = header;
	view.header.seq = 100;
	view.metadata_valid = PW_NDARRAY_FILTER_METADATA_HEADER | PW_NDARRAY_FILTER_METADATA_PROPERTIES;
	spa_assert_se(validate_completed_output(&port, &native, &view) == 0);
	commit_output(&port, &native, &view);
	spa_assert_se(header.seq == 100);
	/* A callback cannot redirect the borrowed descriptor. */
	view.properties.data = NULL;
	spa_assert_se(validate_completed_output(&port, &native, &view) == -EINVAL);

	/* Retained outputs cannot leak the preceding callback's properties. */
	spa_assert_se(project_buffer(&native, &port, true, &view) == 0);
	spa_assert_se(initialize_output_properties(&port, &native) == 0);
	spa_assert_se(spa_meta_properties_is_valid(&metas[1], NULL, 0));
	spa_assert_se(!spa_meta_properties_is_valid(&metas[1], &requirement, 1));
	spa_assert_se(project_buffer(&native, &port, false, &view) == -EINVAL);
	/* Explicitly disabled Properties leave Header-only admission available. */
	port.properties_capacity = 0;
	buffer.n_metas = 2;
	spa_assert_se(project_buffer(&native, &port, false, &view) == 0);
	test_configuration();
	puts("ndarray Properties: borrowed ABI, independent publication sequence, requirements and reuse passed");
	return 0;
}
