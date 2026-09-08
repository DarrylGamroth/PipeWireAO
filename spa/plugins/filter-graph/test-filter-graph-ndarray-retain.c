/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <spa/filter-graph/filter-graph-ndarray.h>

int main(int argc, char *argv[])
{
	struct spa_fgn_graph *graph;
	void *handle;
	char config[1024];
	uint32_t i;
	int res;

	assert(argc == 2);
	handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
	assert(handle == NULL);
	/* Graph instances are destroyed while the opted-in DSO stays mapped. */
	for (i = 0; i < 8; i++) {
		res = snprintf(config, sizeof(config),
			"{ nodes = ["
			" { type = ndarray name = retained plugin = \"%s\""
			"   label = scale-f32 }"
			"] }", argv[1]);
		assert(res > 0 && (size_t)res < sizeof(config));
		assert(spa_fgn_graph_new(config, &graph) == 0);
		spa_fgn_graph_free(graph);

		handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
		assert(handle != NULL);
		assert(dlclose(handle) == 0);
	}
	return EXIT_SUCCESS;
}
