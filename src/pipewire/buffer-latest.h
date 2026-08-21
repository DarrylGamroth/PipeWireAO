/* PipeWire */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_BUFFER_LATEST_H
#define PIPEWIREAO_BUFFER_LATEST_H

#include <stdint.h>

#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Producer-local accounting for bounded latest-buffer acquisition.
 *
 * These counters are written only by the endpoint's exclusive output worker.
 * Concurrent control-thread snapshots are not supported.
 */
struct pw_buffer_latest_stats {
	uint64_t dequeue_attempts;
	uint64_t completions;
	uint64_t buffer_probes;
	uint64_t pool_exhaustions;
	uint64_t submission_reclaims;
	uint64_t submission_withdrawals;
	uint64_t publications;
	uint64_t subscriber_visits;
	uint64_t subscriber_deliveries;
	uint64_t submission_overflows;
	uint64_t subscriber_retirements;
	uint64_t retired_leases;
	uint64_t zero_recipient_publications;
	uint32_t max_buffer_probes;
	uint32_t max_completions;
	uint32_t max_submission_withdrawals;
	uint32_t max_subscriber_visits;
};

#define PW_BUFFER_LATEST_STATS_VERSION_0_SIZE 120u
SPA_STATIC_ASSERT(sizeof(struct pw_buffer_latest_stats) ==
		PW_BUFFER_LATEST_STATS_VERSION_0_SIZE,
		"latest-buffer statistics version 0 ABI");

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIREAO_BUFFER_LATEST_H */
