/* Investigative reproduction for AO-REV-002, not a production test. */
#include <stdio.h>
#include <spa/filter-graph/filter-graph-ndarray.h>

int main(int argc, char **argv)
{
	const char *tails[] = { "] }", "7 ] }", "] } garbage",
		"] links = [7] }", "] workers = {helpers = 0 broken} }" };
	unsigned int i;
	if (argc != 2)
		return 2;
	for (i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
		char config[4096];
		struct spa_fgn_graph *graph = NULL;
		struct spa_fgn_graph_info info = { .struct_size = sizeof(info) };
		int result;
		snprintf(config, sizeof(config),
			"{ nodes = [{type = ndarray name = scale plugin = \"%s\" label = scale-f32} %s",
			argv[1], tails[i]);
		result = spa_fgn_graph_new(config, &graph);
		if (graph != NULL)
			spa_fgn_graph_get_info(graph, &info);
		printf("case=%u result=%d nodes=%u tail=%s\n", i, result, info.n_nodes, tails[i]);
		spa_fgn_graph_free(graph);
	}
	return 0;
}
