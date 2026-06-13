#include "cycleinfo.h"
#include <stdio.h>
#include <string.h>

CycleReport cycle_analyze(TypeTable *tt)
{
	CycleReport r;
	memset(&r, 0, sizeof r);
	r.total_classes = tt ? tt->class_count : 0;
	r.acyclic = 1;   /* Filled in by later tasks; an empty graph is acyclic. */
	return r;
}

void cycle_report_print(const CycleReport *r)
{
	fprintf(stderr,
		"[cycle] classes=%d in_cycles=%d sccs=%d largest=%d weak_candidates=%d ambiguous=%d acyclic=%d\n",
		r->total_classes, r->classes_in_cycles, r->scc_count, r->largest_scc,
		r->weak_edge_candidates, r->ambiguous_edges, r->acyclic);
}
