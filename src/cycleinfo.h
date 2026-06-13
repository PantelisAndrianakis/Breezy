#ifndef CYCLEINFO_H
#define CYCLEINFO_H
#include "types.h"

/* Coverage facts from the static type-reference-graph cycle analysis. Pure
   measurement: cycle_analyze mutates nothing and affects no codegen. */
typedef struct
{
	int total_classes;        /* User + builtin classes considered. */
	int classes_in_cycles;    /* Classes in a non-trivial SCC (size > 1, or a self-loop). */
	int scc_count;            /* Number of non-trivial SCCs. */
	int largest_scc;          /* Node count of the largest non-trivial SCC. */
	int weak_edge_candidates; /* Back-edges that must become weak to break every SCC. */
	int ambiguous_edges;      /* Weak candidates that fall on an interface/base-typed field
	                             (owner inference hardest here). */
	int acyclic;              /* 1 if no non-trivial SCC: collector removable with no weak edges. */
} CycleReport;

CycleReport cycle_analyze(TypeTable *tt);
void        cycle_report_print(const CycleReport *r);

#endif
