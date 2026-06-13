#include "cycleinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Index of a class by name, or -1. Linear scan: class_count is small. */
static int class_index(TypeTable *tt, const char *name)
{
	for (int i = 0; i < tt->class_count; i++)
	{
		if (strcmp(tt->classes[i]->name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

/* Add edge from -> to, plus from -> every subtype of `to` (a field of static type
   `to` may hold any subtype at runtime, and that subtype's fields can close a
   cycle). Interface targets expand to implementers. amb is set when the edge ran
   through an interface or a subtype (owner inference is hardest there). */
static void add_edge(TypeTable *tt, char *adj, int *amb, int from, const char *to_name)
{
	int exact = class_index(tt, to_name);
	int iface = types_is_interface(tt, to_name);
	int n = tt->class_count;
	for (int j = 0; j < n; j++)
	{
		ClassInfo *c = tt->classes[j];
		int matches = (j == exact);
		for (ClassInfo *p = c->parent; p && !matches; p = p->parent)
		{
			if (strcmp(p->name, to_name) == 0) { matches = 1; }
		}
		if (!matches && iface)
		{
			for (int k = 0; k < c->implements_count; k++)
			{
				if (strcmp(c->implements[k], to_name) == 0) { matches = 1; break; }
			}
		}
		if (matches)
		{
			adj[from * n + j] = 1;
			if (j != exact || iface) { amb[from * n + j] = 1; }   /* Reached via subtype/interface. */
		}
	}
}

/* Decompose a managed field type into the class edges it implies. */
static void edges_from_type(TypeTable *tt, char *adj, int *amb, int from, TypeRef *t)
{
	if (!t)
	{
		return;
	}

	switch (t->kind)
	{
	case TY_OBJECT:
		add_edge(tt, adj, amb, from, t->class_name);
		break;
	case TY_ARRAY:
	case TY_MAP:
	case TY_GENERIC:
	case TY_ENTRY:
		edges_from_type(tt, adj, amb, from, t->elem);
		edges_from_type(tt, adj, amb, from, t->elem2);
		break;
	default:
		break;   /* Value types and immutable strings cannot root a reclaimable cycle. */
	}
}

/* Build the class-reference adjacency (and a companion "ambiguous edge" matrix).
   Caller frees both with free(). Returns NULL on a zero-class table. */
static char *build_adjacency(TypeTable *tt, int **amb_out)
{
	int n = tt->class_count;
	if (n == 0)
	{
		*amb_out = NULL;
		return NULL;
	}

	char *adj = calloc((size_t)n * n, sizeof(char));
	int  *amb = calloc((size_t)n * n, sizeof(int));
	for (int i = 0; i < n; i++)
	{
		ClassInfo *c = tt->classes[i];
		for (int f = 0; f < c->field_count; f++)
		{
			if (c->fields[f].is_static)
			{
				continue;   /* Static fields are global slots, not per-object edges. */
			}
			edges_from_type(tt, adj, amb, i, &c->fields[f].type);
		}
	}

	*amb_out = amb;
	return adj;
}

/* Test hook: expose the static adjacency builder. */
char *cycleinfo_test_adjacency(TypeTable *tt, int **amb_out)
{
	return build_adjacency(tt, amb_out);
}

typedef struct
{
	char *adj;
	int  n;
	int  *index;     /* Tarjan discovery index, -1 = unvisited. */
	int  *low;
	int  *onstack;
	int  *stack;
	int   sp;
	int   counter;
	int  *comp;      /* Component id per node. */
	int   ncomp;
	int  *comp_size; /* Size per component. */
} Scc;

static void scc_dfs(Scc *s, int v)
{
	s->index[v] = s->low[v] = s->counter++;
	s->stack[s->sp++] = v;
	s->onstack[v] = 1;

	for (int w = 0; w < s->n; w++)
	{
		if (!s->adj[v * s->n + w])
		{
			continue;
		}

		if (s->index[w] < 0)
		{
			scc_dfs(s, w);
			if (s->low[w] < s->low[v]) { s->low[v] = s->low[w]; }
		}
		else if (s->onstack[w])
		{
			if (s->index[w] < s->low[v]) { s->low[v] = s->index[w]; }
		}
	}

	if (s->low[v] == s->index[v])
	{
		int id = s->ncomp++;
		int sz = 0;
		int w;
		do
		{
			w = s->stack[--s->sp];
			s->onstack[w] = 0;
			s->comp[w] = id;
			sz++;
		} while (w != v);
		s->comp_size[id] = sz;
	}
}

/* Feedback-arc-set approximation: a DFS over the whole graph; any edge to a
   vertex currently on the DFS stack is a back-edge and a weak candidate. Not
   minimal, but a sound measure of how many edges must become weak. */
typedef struct { char *adj; int n; int *state; int *amb; int weak; int ambiguous; } Fas;
/* state: 0=unvisited, 1=on stack, 2=done. */
static void fas_dfs(Fas *f, int v)
{
	f->state[v] = 1;
	for (int w = 0; w < f->n; w++)
	{
		if (!f->adj[v * f->n + w]) { continue; }
		if (f->state[w] == 1)
		{
			f->weak++;                                   /* Back-edge: a weak candidate. */
			if (f->amb[v * f->n + w]) { f->ambiguous++; }
		}
		else if (f->state[w] == 0)
		{
			fas_dfs(f, w);
		}
	}
	f->state[v] = 2;
}

CycleReport cycle_analyze(TypeTable *tt)
{
	CycleReport r;
	memset(&r, 0, sizeof r);
	r.total_classes = tt ? tt->class_count : 0;
	r.acyclic = 1;

	int n = r.total_classes;
	if (n == 0)
	{
		return r;
	}

	int *amb = NULL;
	char *adj = build_adjacency(tt, &amb);

	Scc s;
	s.adj = adj; s.n = n;
	s.index = malloc((size_t)n * sizeof(int));
	s.low = malloc((size_t)n * sizeof(int));
	s.onstack = calloc((size_t)n, sizeof(int));
	s.stack = malloc((size_t)n * sizeof(int));
	s.comp = malloc((size_t)n * sizeof(int));
	s.comp_size = calloc((size_t)n, sizeof(int));
	s.sp = 0; s.counter = 0; s.ncomp = 0;
	for (int i = 0; i < n; i++) { s.index[i] = -1; }

	for (int i = 0; i < n; i++)
	{
		if (s.index[i] < 0) { scc_dfs(&s, i); }
	}

	/* A component is a non-trivial (collectable-cycle) SCC if it has >1 node, or
	   it is a single node with a self-edge. */
	for (int id = 0; id < s.ncomp; id++)
	{
		int sz = s.comp_size[id];
		int nontrivial = (sz > 1);
		if (sz == 1)
		{
			for (int v = 0; v < n; v++)
			{
				if (s.comp[v] == id && adj[v * n + v]) { nontrivial = 1; break; }
			}
		}
		if (nontrivial)
		{
			r.scc_count++;
			if (sz > r.largest_scc) { r.largest_scc = sz; }
		}
	}

	for (int v = 0; v < n; v++)
	{
		int id = s.comp[v];
		int sz = s.comp_size[id];
		int incyc = (sz > 1) || (sz == 1 && adj[v * n + v]);
		if (incyc) { r.classes_in_cycles++; }
	}

	r.acyclic = (r.scc_count == 0);

	Fas f; f.adj = adj; f.n = n; f.amb = amb;
	f.state = calloc((size_t)n, sizeof(int)); f.weak = 0; f.ambiguous = 0;
	for (int i = 0; i < n; i++) { if (f.state[i] == 0) { fas_dfs(&f, i); } }
	r.weak_edge_candidates = f.weak;
	r.ambiguous_edges = f.ambiguous;
	free(f.state);

	free(s.index); free(s.low); free(s.onstack); free(s.stack);
	free(s.comp); free(s.comp_size); free(adj); free(amb);
	return r;
}

void cycle_report_print(const CycleReport *r)
{
	fprintf(stderr,
		"[cycle] classes=%d in_cycles=%d sccs=%d largest=%d weak_candidates=%d ambiguous=%d acyclic=%d\n",
		r->total_classes, r->classes_in_cycles, r->scc_count, r->largest_scc,
		r->weak_edge_candidates, r->ambiguous_edges, r->acyclic);
}
