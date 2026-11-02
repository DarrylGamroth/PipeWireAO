/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */
#include "../pipewire/ndarray-filter.c"
#include "metadata-fixture.h"
#include <pthread.h>

struct invoke_probe {
	struct pw_loop *loop;
	pthread_t owner;
	unsigned calls;
	bool on_owner;
};

static int late_invoke(struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t seq SPA_UNUSED, const void *data SPA_UNUSED,
		size_t size SPA_UNUSED, void *user_data)
{
	struct invoke_probe *probe = user_data;
	probe->calls++;
	probe->on_owner = pthread_equal(probe->owner, pthread_self());
	return 0;
}

static void *queue_late_invoke(void *data)
{
	struct invoke_probe *probe = data;
	spa_assert_se(pw_loop_invoke(probe->loop, late_invoke, SPA_ID_INVALID,
			NULL, 0, false, probe) >= 0);
	return NULL;
}

static void quit_loop(void *data, uint64_t count SPA_UNUSED)
{
	pw_main_loop_quit(data);
}

/* A late data-thread invoke must remain queued after run returns. */
static void test_loop_ownership(struct pw_ndarray_filter *filter)
{
	struct invoke_probe probe = {
		.loop = pw_main_loop_get_loop(filter->main_loop),
		.owner = pthread_self(),
	};
	struct spa_source *event = pw_loop_add_event(probe.loop, quit_loop,
			filter->main_loop);
	pthread_t worker;

	spa_assert_se(event != NULL);
	spa_assert_se(pw_loop_signal_event(probe.loop, event) == 0);
	spa_assert_se(pw_main_loop_run(filter->main_loop) == 0);
	spa_assert_se(pthread_create(&worker, NULL, queue_late_invoke, &probe) == 0);
	spa_assert_se(pthread_join(worker, NULL) == 0);
	spa_assert_se(probe.calls == 0);
	pw_loop_destroy_source(probe.loop, event);
	pw_ndarray_filter_destroy(filter);
	spa_assert_se(probe.calls == 1 && probe.on_owner);
}
static int no_process(void *d SPA_UNUSED, const struct pw_ndarray_filter_buffer *i SPA_UNUSED,
		      uint32_t ni SPA_UNUSED, struct pw_ndarray_filter_buffer *o SPA_UNUSED,
		      uint32_t no SPA_UNUSED)
{
	return 0;
}
int main(void)
{
	uint32_t shape = 1;
	struct pw_ndarray_filter_events events = {.version = PW_VERSION_NDARRAY_FILTER_EVENTS,
						  .process = no_process};
	struct spa_meta_requirement requirement = {SPA_META_AcquisitionIdentity,
						   sizeof(struct spa_meta_acquisition_identity),
						   SPA_META_REQUIREMENT_REQUIRED, 0};
	struct pw_ndarray_filter_port declaration = {
	    .struct_size = sizeof(declaration),
	    .direction = SPA_DIRECTION_INPUT,
	    .name = "input",
	    .format = {.element_type = SPA_ELEMENT_TYPE_F32_LE,
		       .layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
		       .n_dimensions = 1,
		       .shape = &shape},
	    .n_metas = 1,
	    .metas = &requirement};
	struct pw_ndarray_filter_config config = {.struct_size = sizeof(config),
						  .node_name = "metadata-configuration",
						  .n_ports = 1,
						  .ports = &declaration,
						  .events = &events};
	struct pw_ndarray_filter *filter;
	spa_assert_se(pw_ndarray_filter_new(&config, &filter) == 0);
	const struct pw_properties *properties = pw_filter_get_properties(filter->filter, NULL);
	spa_assert_se(
	    spa_streq(pw_properties_get(properties, PW_KEY_HEADER_PTS_CLOCK), "unavailable"));
	spa_assert_se(spa_streq(
	    pw_properties_get(properties, PW_KEY_HEADER_PTS_ABSOLUTE_CONVERSION), "unavailable"));

	requirement.type = 11;
	spa_assert_se(filter->inputs[0]->metas[0].type == SPA_META_AcquisitionIdentity);
	test_loop_ownership(filter);
	spa_assert_se(pw_ndarray_filter_new(&config, &filter) == -EINVAL);
	requirement.type = SPA_META_AcquisitionIdentity;
	requirement.size--;
	spa_assert_se(pw_ndarray_filter_new(&config, &filter) == -EINVAL);
	requirement.size++;
	struct spa_meta_requirement local_state = {SPA_META_Busy, sizeof(struct spa_meta_busy), 0,
						   0};
	struct pw_ndarray_filter_port preserve_ports[] = {declaration, declaration};
	preserve_ports[0].metas = &local_state;
	preserve_ports[1].direction = SPA_DIRECTION_OUTPUT;
	preserve_ports[1].name = "output";
	preserve_ports[1].n_metas = 0;
	preserve_ports[1].metas = NULL;
	preserve_ports[1].propagation = SPA_META_PROPAGATION_PRESERVE;
	struct pw_ndarray_filter_config preserve_config = config;
	preserve_config.n_ports = SPA_N_ELEMENTS(preserve_ports);
	preserve_config.ports = preserve_ports;
	spa_assert_se(pw_ndarray_filter_new(&preserve_config, &filter) == 0);
	pw_ndarray_filter_destroy(filter);
	local_state.type = 0x3f0; /* An opaque fact does require preserved output capacity. */
	spa_assert_se(pw_ndarray_filter_new(&preserve_config, &filter) == -ENOTSUP);
	struct spa_meta_acquisition_identity identity = {
	    .flags = SPA_META_RECORD_VALID, .domain = {1}, .generation = UINT64_MAX, .sequence = 9};
	struct spa_meta_header header = {.seq = 77};
	struct spa_meta metas[] = {{SPA_META_Header, sizeof(header), &header},
				   {SPA_META_AcquisitionIdentity, sizeof(identity), &identity}};
	float payload = 1;
	struct spa_chunk chunk = {.size = sizeof(payload), .stride = sizeof(payload)};
	struct spa_data block = {
	    .type = SPA_DATA_MemPtr, .data = &payload, .maxsize = sizeof(payload), .chunk = &chunk};
	struct spa_buffer buffer = {.n_metas = 2, .metas = metas, .n_datas = 1, .datas = &block};
	struct pw_buffer native = {.buffer = &buffer};
	struct ndarray_port port = {.size = sizeof(payload),
				    .stride = sizeof(payload),
				    .n_metas = 1,
				    .metas = &requirement};
	struct pw_ndarray_filter_buffer view;
	spa_assert_se(project_buffer(&native, &port, false, &view) == 0);
	spa_assert_se(view.n_metas == 2 && view.metas == metas && test_header(&view)->seq == 77);
	header.seq++;
	spa_assert_se(identity.sequence == 9);
	buffer.n_metas = 1;
	spa_assert_se(project_buffer(&native, &port, false, &view) == -EINVAL);
	buffer.n_metas = 2;
	spa_assert_se(initialize_output_metadata(&port, &native) == 0);
	spa_assert_se(identity.flags == 0 && header.seq == 0);
	spa_assert_se(project_buffer(&native, &port, true, &view) == 0);
	spa_assert_se(validate_completed_output(&port, &native, &view) == -EINVAL);
	view.flags = PW_NDARRAY_FILTER_BUFFER_FLAG_OUTPUT_UNAVAILABLE;
	spa_assert_se(validate_completed_output(&port, &native, &view) == 0);
	view.flags = 0;
	identity = (struct spa_meta_acquisition_identity){
	    .flags = SPA_META_RECORD_VALID, .domain = {1}, .generation = UINT64_MAX, .sequence = 9};
	test_header(&view)->seq = 100;
	spa_assert_se(validate_completed_output(&port, &native, &view) == 0);
	commit_output(&port, &native, &view);
	spa_assert_se(header.seq == 100);
	view.metas = NULL;
	spa_assert_se(validate_completed_output(&port, &native, &view) == -EINVAL);
	/* Raw 4090 bytes require 4104 bytes after alignment: reject before callback. */
	uint64_t large[508] = {0}, small = 0;
	struct spa_meta over_budget[] = {
	    {SPA_META_Header, sizeof(header), &header}, {0x3fe, 4057, large}, {0x3ff, 1, &small}};
	struct memory_region regions[MAX_BUFFER_REGIONS];
	uint32_t n_regions = 0;
	buffer.metas = over_budget;
	buffer.n_metas = SPA_N_ELEMENTS(over_budget);
	port.format.element_type = SPA_ELEMENT_TYPE_F32_LE;
	spa_assert_se(!spa_meta_records_layout_validate(over_budget, buffer.n_metas));
	spa_assert_se(collect_buffer_regions(&native, &port, true, regions, &n_regions) == -EINVAL);
	buffer.metas = metas;
	buffer.n_metas = SPA_N_ELEMENTS(metas);
	events.version = 4;
	spa_assert_se(pw_ndarray_filter_new(&config, &filter) == -EINVAL);
	puts("ndarray metadata: cold exact layouts, borrowed canonical Header, unsigned identity, "
	     "missing/invalid reuse and retired ABI rejection passed");
	return 0;
}
