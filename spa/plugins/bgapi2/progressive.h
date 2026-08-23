/* SPDX-License-Identifier: MIT */
#ifndef SPA_BGAPI2_PROGRESSIVE_H
#define SPA_BGAPI2_PROGRESSIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool bgapi2_progressive_committed_prefix(uint64_t size_filled,
		uint32_t payload_size, uint32_t commit_granularity,
		uint32_t *committed)
{
	if (committed == NULL || payload_size == 0 || commit_granularity == 0 ||
			commit_granularity > payload_size || size_filled > payload_size)
		return false;
	if (size_filled == payload_size)
		*committed = payload_size;
	else
		*committed = (uint32_t)(size_filled / commit_granularity) *
				commit_granularity;
	return true;
}

#endif
