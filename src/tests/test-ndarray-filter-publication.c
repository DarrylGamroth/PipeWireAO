/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <stdarg.h>

#include <spa/param/props.h>

/* Compile the implementation into this test so its parameter publication and
 * main-loop error calls can be replaced deterministically without a production
 * fault-injection API. */
#define pw_filter_update_params test_filter_update_params
#define pw_filter_set_error test_filter_set_error
#define pw_main_loop_quit test_main_loop_quit
#define pw_main_loop_run test_main_loop_run
#include "../pipewire/ndarray-filter.c"
#undef pw_main_loop_run
#undef pw_main_loop_quit
#undef pw_filter_set_error
#undef pw_filter_update_params

static int update_result;
static uint32_t update_calls;
static uint32_t last_n_params;
static uint32_t last_owner_props;
static uint32_t last_prop_info;
static bool last_prop_info_alpha;
static bool last_prop_info_beta;
static bool last_run_status;
static bool last_reset_status;
static struct pw_ao_run_control_status observed_run_status;
static struct pw_ao_reset_control_status observed_reset_status;
static uint32_t set_error_calls;
static uint32_t quit_calls;
static uint32_t reset_calls;
static uint32_t owner_props_calls;
static uint8_t owner_props_buffer[256];
static uint8_t stable_prop_info_buffers[2][256];

static void reset_update_observation(void)
{
	update_calls = 0;
	last_n_params = 0;
	last_owner_props = 0;
	last_prop_info = 0;
	last_prop_info_alpha = false;
	last_prop_info_beta = false;
	last_run_status = false;
	last_reset_status = false;
	spa_zero(observed_run_status);
	spa_zero(observed_reset_status);
}

int test_filter_update_params(struct pw_filter *filter, void *port_data,
		const struct spa_pod **params, uint32_t n_params)
{
	uint32_t i;

	update_calls++;
	last_n_params = n_params;
	for (i = 0; i < n_params; i++) {
		if (SPA_POD_OBJECT_ID(params[i]) == SPA_PARAM_PropInfo) {
			const struct spa_pod_prop *name = spa_pod_find_prop(params[i],
					NULL, SPA_PROP_INFO_name);
			const char *value = NULL;

			spa_assert_se(name != NULL);
			spa_assert_se(spa_pod_get_string(&name->value, &value) == 0);
			last_prop_info++;
			last_prop_info_alpha |= spa_streq(value, "owner:alpha");
			last_prop_info_beta |= spa_streq(value, "owner:beta");
		} else {
			struct pw_ao_run_control_status run_status;
			struct pw_ao_reset_control_status reset_status;

			spa_assert_se(SPA_POD_OBJECT_ID(params[i]) == SPA_PARAM_Props);
			if (pw_ao_run_control_parse_status(params[i], &run_status) == 0) {
				last_run_status = true;
				observed_run_status = run_status;
			} else if (pw_ao_reset_control_parse_status(params[i],
					&reset_status) == 0) {
				last_reset_status = true;
				observed_reset_status = reset_status;
			} else {
				last_owner_props++;
			}
		}
	}
	return update_result;
}

int test_filter_set_error(struct pw_filter *filter, int res,
		const char *error, ...)
{
	set_error_calls++;
	return 0;
}

int test_main_loop_quit(struct pw_main_loop *loop)
{
	quit_calls++;
	return 0;
}

int test_main_loop_run(struct pw_main_loop *loop)
{
	return 0;
}

static int reset_owner(void *data)
{
	reset_calls++;
	return 0;
}

static int get_owner_props(void *data SPA_UNUSED,
		const struct spa_pod **props)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(owner_props_buffer,
			sizeof(owner_props_buffer));
	struct spa_pod_frame object;

	owner_props_calls++;
	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_add(&builder,
			SPA_PROP_mute, SPA_POD_Bool(false), 0);
	*props = spa_pod_builder_pop(&builder, &object);
	return *props == NULL ? -ENOSPC : 0;
}

static int enum_stable_prop_info(void *data SPA_UNUSED, uint32_t index,
		const struct spa_pod **info)
{
	struct spa_pod_builder builder;
	struct spa_pod_frame object;

	if (index >= 2)
		return -ENOENT;
	spa_pod_builder_init(&builder, stable_prop_info_buffers[index],
			sizeof(stable_prop_info_buffers[index]));
	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(&builder,
			SPA_PROP_INFO_name, SPA_POD_String(index == 0
				? "owner:alpha" : "owner:beta"),
			SPA_PROP_INFO_type, SPA_POD_Float(0.0f), 0);
	*info = spa_pod_builder_pop(&builder, &object);
	return *info == NULL ? -ENOSPC : 0;
}

static void init_filter(struct pw_ndarray_filter *filter, uint32_t flags)
{
	spa_zero(*filter);
	filter->main_loop = (struct pw_main_loop *)(uintptr_t)1;
	filter->filter = (struct pw_filter *)(uintptr_t)1;
	filter->flags = flags;
	filter->connected = true;
	filter->actual_state = PW_AO_RUN_CONTROL_STATE_STOPPED;
	atomic_init(&filter->error, 0);
}

static void expect_complete_props(int64_t run_token, int run_result,
		enum pw_ao_run_control_state actual_state, int64_t reset_token,
		int reset_result)
{
	spa_assert_se(update_calls == 1);
	spa_assert_se(last_n_params == 3);
	spa_assert_se(last_owner_props == 1);
	spa_assert_se(last_run_status);
	spa_assert_se(last_reset_status);
	spa_assert_se(observed_run_status.completed_token == run_token);
	spa_assert_se(observed_run_status.result == run_result);
	spa_assert_se(observed_run_status.actual_state == actual_state);
	spa_assert_se(observed_reset_status.completed_token == reset_token);
	spa_assert_se(observed_reset_status.result == reset_result);
}

static void test_stable_prop_info_is_batched(void)
{
	struct pw_ndarray_filter filter;

	init_filter(&filter, PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES);
	filter.events.enum_prop_info = enum_stable_prop_info;
	update_result = 0;
	reset_update_observation();

	spa_assert_se(publish_owner_prop_info(&filter) == 0);
	spa_assert_se(update_calls == 1);
	spa_assert_se(last_n_params == 2);
	spa_assert_se(last_prop_info == 2);
	spa_assert_se(last_prop_info_alpha);
	spa_assert_se(last_prop_info_beta);
}

static void test_complete_props_are_republished_together(void)
{
	struct pw_ndarray_filter filter;

	init_filter(&filter, PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES |
			PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL |
			PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL);
	filter.events.get_props = get_owner_props;
	filter.completed_token = 7;
	filter.run_control_result = 19;
	filter.completed_reset_token = 8;
	filter.reset_result = 20;
	update_result = 0;

	atomic_store_explicit(&filter.properties_pending, true,
			memory_order_release);
	reset_update_observation();
	properties_event(&filter, 0);
	expect_complete_props(7, 19, PW_AO_RUN_CONTROL_STATE_STOPPED, 8, 20);

	reset_update_observation();
	spa_assert_se(publish_run_control_status(&filter, 51, -EBUSY,
			PW_AO_RUN_CONTROL_STATE_RUNNING) == 0);
	expect_complete_props(51, -EBUSY, PW_AO_RUN_CONTROL_STATE_RUNNING, 8, 20);

	reset_update_observation();
	spa_assert_se(publish_reset_control_status(&filter, 52, -EALREADY) == 0);
	expect_complete_props(51, -EBUSY, PW_AO_RUN_CONTROL_STATE_RUNNING,
			52, -EALREADY);
	spa_assert_se(owner_props_calls == 3);
}

static void test_rejected_run_status_failure_is_terminal(void)
{
	struct pw_ndarray_filter filter;
	uint8_t data[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, sizeof(data));
	struct spa_pod *request;

	init_filter(&filter, PW_NDARRAY_FILTER_FLAG_OWNER_PROPERTIES |
			PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL |
			PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL);
	filter.events.get_props = get_owner_props;
	filter.last_request_token = 42;
	filter.completed_token = 7;
	filter.run_control_result = 19;
	filter.completed_reset_token = 8;
	filter.reset_result = 20;
	update_result = -ENOMEM;
	set_error_calls = quit_calls = 0;
	reset_update_observation();
	request = pw_ao_run_control_build_request(&builder, 42,
			PW_AO_RUN_CONTROL_STATE_RUNNING);
	spa_assert_se(request != NULL);

	filter_param_changed(&filter, NULL, SPA_PARAM_Props, request);

	spa_assert_se(atomic_load_explicit(&filter.error,
			memory_order_acquire) == -ENOMEM);
	spa_assert_se(filter.completed_token == 7);
	spa_assert_se(filter.run_control_result == 19);
	spa_assert_se(filter.completed_reset_token == 8);
	spa_assert_se(filter.reset_result == 20);
	expect_complete_props(42, -EALREADY, PW_AO_RUN_CONTROL_STATE_STOPPED, 8, 20);
	spa_assert_se(set_error_calls == 1);
	spa_assert_se(quit_calls == 1);
	spa_assert_se(pw_ndarray_filter_run(&filter) == -ENOMEM);
}

static void test_applied_reset_status_failure_is_terminal(void)
{
	struct pw_ndarray_filter filter;
	uint8_t data[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, sizeof(data));
	struct spa_pod *request;

	init_filter(&filter, PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL);
	filter.events.reset = reset_owner;
	filter.completed_reset_token = 7;
	filter.reset_result = 19;
	update_result = -EIO;
	set_error_calls = quit_calls = reset_calls = 0;
	request = pw_ao_reset_control_build_request(&builder, 43);
	spa_assert_se(request != NULL);

	filter_param_changed(&filter, NULL, SPA_PARAM_Props, request);

	spa_assert_se(reset_calls == 1);
	spa_assert_se(filter.last_reset_token == 43);
	spa_assert_se(filter.completed_reset_token == 7);
	spa_assert_se(filter.reset_result == 19);
	spa_assert_se(atomic_load_explicit(&filter.error,
			memory_order_acquire) == -EIO);
	spa_assert_se(set_error_calls == 1);
	spa_assert_se(quit_calls == 1);
	spa_assert_se(pw_ndarray_filter_run(&filter) == -EIO);
}

static void test_successful_status_publication_commits_completion(void)
{
	struct pw_ndarray_filter filter;

	init_filter(&filter, PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL |
			PW_NDARRAY_FILTER_FLAG_OWNER_RESET_CONTROL);
	update_result = 0;
	spa_assert_se(publish_run_control_status(&filter, 51, -EBUSY,
			PW_AO_RUN_CONTROL_STATE_RUNNING) == 0);
	spa_assert_se(filter.completed_token == 51);
	spa_assert_se(filter.run_control_result == -EBUSY);
	spa_assert_se(filter.actual_state == PW_AO_RUN_CONTROL_STATE_RUNNING);
	spa_assert_se(publish_reset_control_status(&filter, 52, -EALREADY) == 0);
	spa_assert_se(filter.completed_reset_token == 52);
	spa_assert_se(filter.reset_result == -EALREADY);
}

int main(int argc SPA_UNUSED, char *argv[] SPA_UNUSED)
{
	test_stable_prop_info_is_batched();
	test_complete_props_are_republished_together();
	test_rejected_run_status_failure_is_terminal();
	test_applied_reset_status_failure_is_terminal();
	test_successful_status_publication_commits_completion();
	return 0;
}
