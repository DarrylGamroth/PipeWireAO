/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <assert.h>

#include "../modules/module-ndarray-filter-chain-parameter.h"

struct pw_buffer {
	unsigned int value;
};

int main(int argc, char *argv[])
{
	struct ndarray_parameter_handoff first, second;
	struct pw_buffer first_buffer = { .value = 1 };
	struct pw_buffer next_buffer = { .value = 2 };
	struct pw_buffer second_buffer = { .value = 3 };

	(void)argc;
	(void)argv;
	ndarray_parameter_handoff_init(&first);
	ndarray_parameter_handoff_init(&second);

	assert(ndarray_parameter_handoff_schedule(&first, &first_buffer));
	assert(ndarray_parameter_handoff_claim(&first));
	assert(!ndarray_parameter_handoff_claim(&first));
	ndarray_parameter_handoff_complete(&first);

	/* A wake for another port must not re-enter completed storage while the
	 * data-loop owner has not returned it yet. */
	assert(ndarray_parameter_handoff_schedule(&second, &second_buffer));
	assert(!ndarray_parameter_handoff_claim(&first));
	assert(ndarray_parameter_handoff_claim(&second));
	assert(ndarray_parameter_handoff_pending(&first) == &first_buffer);
	assert(!ndarray_parameter_handoff_schedule(&first, &next_buffer));
	assert(ndarray_parameter_handoff_take_completed(&first) == &first_buffer);
	first_buffer.value = 99;
	assert(!ndarray_parameter_handoff_claim(&first));

	assert(ndarray_parameter_handoff_schedule(&first, &next_buffer));
	assert(ndarray_parameter_handoff_claim(&first));
	ndarray_parameter_handoff_mark_retry(&first);
	assert(ndarray_parameter_handoff_rearm_retry(&first));
	assert(!ndarray_parameter_handoff_rearm_retry(&first));
	assert(ndarray_parameter_handoff_claim(&first));
	ndarray_parameter_handoff_restore_retry(&first);
	assert(!ndarray_parameter_handoff_claim(&first));
	assert(ndarray_parameter_handoff_rearm_retry(&first));
	assert(ndarray_parameter_handoff_claim(&first));
	assert(ndarray_parameter_handoff_cancel(&first) == &next_buffer);
	assert(ndarray_parameter_handoff_pending(&first) == NULL);

	ndarray_parameter_handoff_complete(&second);
	assert(ndarray_parameter_handoff_take_completed(&second) == &second_buffer);
	return 0;
}
