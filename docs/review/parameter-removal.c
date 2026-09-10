/* Investigative reproduction for AO-REV-001, not a production test. */
#define main admission_test_main
#include "../../src/tests/test-ndarray-filter-admission.c"
#undef main
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_parameter(void *data, uint32_t port,
		const struct pw_ndarray_filter_buffer *view)
{
	(void)data;
	(void)port;
	volatile float value = *(const volatile float *)view->data;
	(void)value;
	return 0;
}

int main(void)
{
	struct test_fixture fixture;
	struct test_buffer parameter;
	struct port_data port_data;
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	float *payload = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int status;
	pid_t child;
	struct rlimit no_core = { 0, 0 };

	spa_assert_se(payload != MAP_FAILED);
	*payload = 7.0f;
	pw_init(NULL, NULL);
	init_fixture(&fixture, PW_NDARRAY_FILTER_FLAG_NONE);
	fixture.input.flags = PW_NDARRAY_FILTER_PORT_FLAG_PARAMETER;
	fixture.filter.events.update_parameter = read_parameter;
	init_buffer(&parameter, 7.0f);
	parameter.data.data = payload;
	spa_assert_se(project_parameter_buffer(&fixture.input, &parameter.pw) == 0);
	atomic_store(&fixture.input.pending_parameter, &parameter.pw);
	fixture.filter.filter = NULL;
	port_data.port = &fixture.input;
	filter_remove_buffer(&fixture.filter, &port_data, &parameter.pw);
	printf("pending_after_remove=%d prepared_after_remove=%d error=%d\n",
			atomic_load(&fixture.input.pending_parameter) == &parameter.pw,
			atomic_load(&fixture.filter.prepared),
			atomic_load(&fixture.filter.error));
	fflush(stdout);

	child = fork();
	spa_assert_se(child >= 0);
	if (child == 0) {
		setrlimit(RLIMIT_CORE, &no_core);
		spa_assert_se(munmap(payload, page_size) == 0);
		update_parameter(&fixture.input);
		_exit(0);
	}
	spa_assert_se(waitpid(child, &status, 0) == child);
	printf("worker_after_unmap_signal=%d\n",
			WIFSIGNALED(status) ? WTERMSIG(status) : 0);
	spa_assert_se(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
	munmap(payload, page_size);
	clear_fixture(&fixture);
	pw_deinit();
	return 0;
}
