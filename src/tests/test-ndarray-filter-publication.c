/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <stdarg.h>

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
static uint32_t set_error_calls;
static uint32_t quit_calls;
static uint32_t reset_calls;

int test_filter_update_params(struct pw_filter *filter, void *port_data,
		const struct spa_pod **params, uint32_t n_params)
{
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

static void test_rejected_run_status_failure_is_terminal(void)
{
	struct pw_ndarray_filter filter;
	uint8_t data[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(data, sizeof(data));
	struct spa_pod *request;

	init_filter(&filter, PW_NDARRAY_FILTER_FLAG_OWNER_RUN_CONTROL);
	filter.last_request_token = 42;
	filter.completed_token = 7;
	filter.run_control_result = 19;
	update_result = -ENOMEM;
	set_error_calls = quit_calls = 0;
	request = pw_ao_run_control_build_request(&builder, 42,
			PW_AO_RUN_CONTROL_STATE_RUNNING);
	spa_assert_se(request != NULL);

	filter_param_changed(&filter, NULL, SPA_PARAM_Props, request);

	spa_assert_se(atomic_load_explicit(&filter.error,
			memory_order_acquire) == -ENOMEM);
	spa_assert_se(filter.completed_token == 7);
	spa_assert_se(filter.run_control_result == 19);
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
	test_rejected_run_status_failure_is_terminal();
	test_applied_reset_status_failure_is_terminal();
	test_successful_status_publication_commits_completion();
	return 0;
}
