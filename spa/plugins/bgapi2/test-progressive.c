/* SPDX-License-Identifier: MIT */
#include "progressive.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>

int main(void)
{
	uint32_t committed = UINT32_MAX;

	assert(bgapi2_progressive_committed_prefix(0, 400, 40, &committed));
	assert(committed == 0);
	assert(bgapi2_progressive_committed_prefix(39, 400, 40, &committed));
	assert(committed == 0);
	assert(bgapi2_progressive_committed_prefix(40, 400, 40, &committed));
	assert(committed == 40);
	assert(bgapi2_progressive_committed_prefix(399, 400, 40, &committed));
	assert(committed == 360);
	assert(bgapi2_progressive_committed_prefix(400, 400, 40, &committed));
	assert(committed == 400);
	assert(!bgapi2_progressive_committed_prefix(401, 400, 40, &committed));
	assert(!bgapi2_progressive_committed_prefix(0, 0, 40, &committed));
	assert(!bgapi2_progressive_committed_prefix(0, 400, 0, &committed));
	assert(!bgapi2_progressive_committed_prefix(0, 40, 80, &committed));
	assert(!bgapi2_progressive_committed_prefix(0, 400, 40, NULL));
	return 0;
}
