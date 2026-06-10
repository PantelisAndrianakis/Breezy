#include "regalloc.h"
#include <stdlib.h>
#include <string.h>

/* Allocatable GP registers. rax/rcx/rdx are deliberately absent: they stay
   scratch for idiv (rax/rdx), shift counts (cl), and spill reloads. */
static const char *RA_REGS[RA_NREGS] =
	{ "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };

const char *ra_reg_name(int i)
{
	return RA_REGS[i];
}

int ra_is_callee_saved(int i, int linux_target)
{
	const char *r = RA_REGS[i];
	if (!strcmp(r, "rbx") || !strcmp(r, "r12") || !strcmp(r, "r13")
		|| !strcmp(r, "r14") || !strcmp(r, "r15"))
	{
		return 1;   /* Callee-saved on both ABIs. */
	}

	if (!linux_target && (!strcmp(r, "rsi") || !strcmp(r, "rdi")))
	{
		return 1;   /* Callee-saved on Win64 only. */
	}

	return 0;
}

/* ---- value model ---------------------------------------------------------- */

static int local_index(const IRAlloc *a, long long disp)
{
	for (int k = 0; k < a->nlocal; k++)
	{
		if (a->local_disp[k] == disp)
		{
			return k;
		}
	}

	return -1;
}

/* The value defined (or -1) and the values used by one instruction. A frame
   load reads its local and defines its dst vreg; a frame store defines its local
   and reads the stored vreg; everything else uses a/b/c and defines dst. */
static void instr_def_use(const IRAlloc *a, const IRInstr *in, int *def, int uses[3], int *nuse)
{
	*def = -1;
	*nuse = 0;

	if (in->op == IR_LOAD && in->is_frame)
	{
		*def = in->dst;
		uses[(*nuse)++] = a->vreg_count + local_index(a, in->disp);
		return;
	}

	if (in->op == IR_STORE && in->is_frame)
	{
		*def = a->vreg_count + local_index(a, in->disp);
		if (in->c != IR_NO_REG)
		{
			uses[(*nuse)++] = in->c;
		}

		return;
	}

	*def = (in->dst != IR_NO_REG) ? in->dst : -1;
	if (in->a != IR_NO_REG)
	{
		uses[(*nuse)++] = in->a;
	}

	if (in->b != IR_NO_REG)
	{
		uses[(*nuse)++] = in->b;
	}

	if (in->c != IR_NO_REG)
	{
		uses[(*nuse)++] = in->c;
	}
}

/* ---- accessors ------------------------------------------------------------ */

int ra_vreg_reg(const IRAlloc *a, IRReg v)
{
	return a->val_reg[v];
}

int ra_vreg_slot(const IRAlloc *a, IRReg v)
{
	return a->val_slot[v];
}

int ra_local_reg(const IRAlloc *a, long long disp)
{
	int k = local_index(a, disp);
	return (k < 0) ? RA_SPILLED : a->val_reg[a->vreg_count + k];
}

int ra_local_slot(const IRAlloc *a, long long disp)
{
	int k = local_index(a, disp);
	return (k < 0) ? 0 : a->val_slot[a->vreg_count + k];
}

int ra_debug_max_interval(const IRAlloc *a)
{
	int m = 0;
	for (int v = 0; v < a->nval; v++)
	{
		int span = a->iend[v] - a->istart[v];
		if (a->iend[v] >= 0 && span > m)
		{
			m = span;
		}
	}

	return m;
}

void ra_free(IRAlloc *a)
{
	if (!a)
	{
		return;
	}

	free(a->val_reg);
	free(a->val_slot);
	free(a->local_disp);
	free(a->istart);
	free(a->iend);
	free(a);
}

/* ---- liveness + intervals ------------------------------------------------- */

/* Collect the distinct frame-local offsets referenced by is_frame loads/stores. */
static void collect_locals(IRFunc *f, IRAlloc *a)
{
	int cap = 8;
	a->local_disp = malloc((size_t)cap * sizeof(long long));
	a->nlocal = 0;
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			if ((in->op == IR_LOAD || in->op == IR_STORE) && in->is_frame)
			{
				if (local_index(a, in->disp) < 0)
				{
					if (a->nlocal == cap)
					{
						cap *= 2;
						a->local_disp = realloc(a->local_disp, (size_t)cap * sizeof(long long));
					}

					a->local_disp[a->nlocal++] = in->disp;
				}
			}
		}
	}
}

/* ---- coalescing graph colouring ------------------------------------------- */

static int uf_find(int *uf, int x)
{
	while (uf[x] != x)
	{
		uf[x] = uf[uf[x]];   /* Path halving. */
		x = uf[x];
	}

	return x;
}

/* A use/def at loop depth d weighs 10^d, so a value touched inside an inner loop
   dominates a loop-invariant one - the allocator colours (keeps in a register) the
   heaviest values first and spills the cheapest. Depth is capped so the weight sum
   stays well inside a long long. */
static long long depth_weight(int d)
{
	if (d > 9)
	{
		d = 9;
	}

	long long w = 1;
	for (int i = 0; i < d; i++)
	{
		w *= 10;
	}

	return w;
}

/* Assign each value a register (or a spill slot) by graph colouring with copy
   coalescing. Interference is built precisely from per-point liveness (walking
   each block backward from its live_out set), so a value's live range carries the
   natural holes that hole-free intervals lose - a frame local read into a temp and
   then recomputed does not conflict with that temp. Copies (IR_MOVE and the frame
   IR_LOAD/IR_STORE that shuttle a local through a vreg) are recorded as coalesce
   candidates: when the two sides do not interfere they are unioned onto one node,
   so the load/store/move emits nothing. Colouring is greedy in live-range start
   order over the coalesced graph; a node that finds no free register spills. This
   is what lets `local = local*c1 + c2` update the local in place instead of
   shuttling it through a chain of temps. */
static void ra_color(IRFunc *f, IRAlloc *a, const char *live_out, int bw)
{
	int nval = a->nval;
	if (nval == 0)
	{
		a->spill_bytes = 0;
		return;
	}

	char *interf = calloc((size_t)nval * nval, 1);
	int cpcap = 16;
	int ncp = 0;
	int *cp_d = malloc((size_t)cpcap * sizeof(int));
	int *cp_s = malloc((size_t)cpcap * sizeof(int));
	char *live = malloc((size_t)nval);
	long long *weight = calloc((size_t)nval, sizeof(long long));   /* Per-value spill weight. */

	/* Build interference + copy lists from per-point liveness, and accumulate each
	   value's loop-depth-weighted use/def count. */
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		long long fac = depth_weight(blk->depth);
		memcpy(live, live_out + (size_t)b * bw, (size_t)nval);
		for (int i = blk->count - 1; i >= 0; i--)
		{
			IRInstr *in = &blk->instrs[i];
			int def;
			int uses[3];
			int nuse;
			instr_def_use(a, in, &def, uses, &nuse);
			if (def >= 0)
			{
				weight[def] += fac;
			}

			for (int u = 0; u < nuse; u++)
			{
				if (uses[u] >= 0)
				{
					weight[uses[u]] += fac;
				}
			}

			int is_copy = (in->op == IR_MOVE)
						  || ((in->op == IR_LOAD || in->op == IR_STORE) && in->is_frame);
			int src = (is_copy && nuse == 1) ? uses[0] : -1;

			if (def >= 0)
			{
				for (int w = 0; w < nval; w++)
				{
					if (live[w] && w != def && !(src >= 0 && w == src))
					{
						interf[(size_t)def * nval + w] = 1;
						interf[(size_t)w * nval + def] = 1;
					}
				}

				if (src >= 0 && src != def)
				{
					if (ncp == cpcap)
					{
						cpcap *= 2;
						cp_d = realloc(cp_d, (size_t)cpcap * sizeof(int));
						cp_s = realloc(cp_s, (size_t)cpcap * sizeof(int));
					}

					cp_d[ncp] = def;
					cp_s[ncp] = src;
					ncp++;
				}

				live[def] = 0;
			}

			for (int u = 0; u < nuse; u++)
			{
				if (uses[u] >= 0)
				{
					live[uses[u]] = 1;
				}
			}
		}
	}

	/* Coalesce: union a copy's two ends when no member of either group conflicts. */
	int *uf = malloc((size_t)nval * sizeof(int));
	for (int i = 0; i < nval; i++)
	{
		uf[i] = i;
	}

	for (int c = 0; c < ncp; c++)
	{
		int A = uf_find(uf, cp_d[c]);
		int B = uf_find(uf, cp_s[c]);
		if (A == B)
		{
			continue;
		}

		int bad = 0;
		for (int p = 0; p < nval && !bad; p++)
		{
			if (uf_find(uf, p) != A)
			{
				continue;
			}

			for (int q = 0; q < nval; q++)
			{
				if (uf_find(uf, q) == B && interf[(size_t)p * nval + q])
				{
					bad = 1;
					break;
				}
			}
		}

		if (!bad)
		{
			uf[A] = B;
		}
	}

	/* Interference between coalesced roots. */
	char *rintf = calloc((size_t)nval * nval, 1);
	for (int p = 0; p < nval; p++)
	{
		for (int q = p + 1; q < nval; q++)
		{
			if (interf[(size_t)p * nval + q])
			{
				int A = uf_find(uf, p);
				int B = uf_find(uf, q);
				if (A != B)
				{
					rintf[(size_t)A * nval + B] = 1;
					rintf[(size_t)B * nval + A] = 1;
				}
			}
		}
	}

	/* Live roots, with each root's spill weight (sum of member weights) and earliest
	   start. Colouring visits the heaviest roots first so the hottest values win
	   registers and the cheapest spill (loop-depth-aware spilling). */
	char *root_live = calloc((size_t)nval, 1);
	int *rstart = malloc((size_t)nval * sizeof(int));
	long long *rweight = calloc((size_t)nval, sizeof(long long));
	for (int r = 0; r < nval; r++)
	{
		rstart[r] = 0x7fffffff;
	}

	for (int v = 0; v < nval; v++)
	{
		int r = uf_find(uf, v);
		if (a->iend[v] >= 0)
		{
			root_live[r] = 1;
		}

		rweight[r] += weight[v];
		if (a->istart[v] < rstart[r])
		{
			rstart[r] = a->istart[v];
		}
	}

	int *order = malloc((size_t)nval * sizeof(int));
	int no = 0;
	for (int r = 0; r < nval; r++)
	{
		if (uf_find(uf, r) == r && root_live[r])
		{
			order[no++] = r;
		}
	}

	/* Sort by weight descending; ties by earliest start (deterministic). */
	for (int i = 1; i < no; i++)
	{
		int x = order[i];
		int j = i - 1;
		while (j >= 0 && (rweight[order[j]] < rweight[x]
						  || (rweight[order[j]] == rweight[x] && rstart[order[j]] > rstart[x])))
		{
			order[j + 1] = order[j];
			j--;
		}

		order[j + 1] = x;
	}

	/* Greedy colour: lowest register not taken by an already-coloured neighbour. */
	int *color = malloc((size_t)nval * sizeof(int));
	for (int r = 0; r < nval; r++)
	{
		color[r] = -1;
	}

	for (int i = 0; i < no; i++)
	{
		int r = order[i];
		char used[RA_NREGS];
		memset(used, 0, sizeof used);
		for (int k = 0; k < i; k++)
		{
			int r2 = order[k];
			if (color[r2] >= 0 && rintf[(size_t)r * nval + r2])
			{
				used[color[r2]] = 1;
			}
		}

		for (int rr = 0; rr < RA_NREGS; rr++)
		{
			if (!used[rr])
			{
				color[r] = rr;
				break;
			}
		}
	}

	/* Project the root colours back onto every value. */
	for (int v = 0; v < nval; v++)
	{
		if (a->iend[v] < 0)
		{
			a->val_reg[v] = RA_SPILLED;   /* Never live: no register, no slot. */
			continue;
		}

		int col = color[uf_find(uf, v)];
		if (col >= 0)
		{
			a->val_reg[v] = col;
			a->used_reg[col] = 1;
		}
		else
		{
			a->val_reg[v] = RA_SPILLED;
		}
	}

	/* One spill slot per spilled root, shared by its coalesced members. */
	int *root_slot = malloc((size_t)nval * sizeof(int));
	for (int r = 0; r < nval; r++)
	{
		root_slot[r] = -1;
	}

	int nspill = 0;
	for (int v = 0; v < nval; v++)
	{
		if (a->val_reg[v] == RA_SPILLED && a->iend[v] >= 0)
		{
			int r = uf_find(uf, v);
			if (root_slot[r] < 0)
			{
				root_slot[r] = nspill++;
			}

			a->val_slot[v] = root_slot[r];
		}
	}

	a->spill_bytes = nspill * 8;

	free(interf);
	free(cp_d);
	free(cp_s);
	free(live);
	free(weight);
	free(uf);
	free(rintf);
	free(root_live);
	free(rstart);
	free(rweight);
	free(order);
	free(color);
	free(root_slot);
}

IRAlloc *ra_run(IRFunc *f)
{
	IRAlloc *a = calloc(1, sizeof(IRAlloc));
	a->vreg_count = f->vreg_count;
	collect_locals(f, a);
	a->nval = a->vreg_count + a->nlocal;
	int nval = a->nval > 0 ? a->nval : 1;

	a->val_reg = malloc((size_t)nval * sizeof(int));
	a->val_slot = calloc((size_t)nval, sizeof(int));
	a->istart = malloc((size_t)nval * sizeof(int));
	a->iend = malloc((size_t)nval * sizeof(int));
	for (int v = 0; v < nval; v++)
	{
		a->val_reg[v] = RA_SPILLED;
		a->istart[v] = 0x7fffffff;
		a->iend[v] = -1;
	}

	int nb = f->block_count;
	int bw = a->nval;   /* Bitset width in values. */

	/* Linear position of each block's first instruction, and block lengths. */
	int *first_pos = malloc((size_t)(nb > 0 ? nb : 1) * sizeof(int));
	int pos = 0;
	for (int b = 0; b < nb; b++)
	{
		first_pos[b] = pos;
		pos += f->blocks[b].count;
	}

	/* gen/kill per block (use-before-def / defined). */
	char *gen = calloc((size_t)(nb * bw > 0 ? nb * bw : 1), 1);
	char *kill = calloc((size_t)(nb * bw > 0 ? nb * bw : 1), 1);
	for (int b = 0; b < nb; b++)
	{
		IRBlock *blk = &f->blocks[b];
		char *g = gen + (size_t)b * bw;
		char *k = kill + (size_t)b * bw;
		for (int i = 0; i < blk->count; i++)
		{
			int def;
			int uses[3];
			int nuse;
			instr_def_use(a, &blk->instrs[i], &def, uses, &nuse);
			for (int u = 0; u < nuse; u++)
			{
				if (uses[u] >= 0 && !k[uses[u]])
				{
					g[uses[u]] = 1;   /* Used before any def in this block. */
				}
			}

			if (def >= 0)
			{
				k[def] = 1;
			}
		}
	}

	/* Backward liveness fixpoint. */
	char *live_in = calloc((size_t)(nb * bw > 0 ? nb * bw : 1), 1);
	char *live_out = calloc((size_t)(nb * bw > 0 ? nb * bw : 1), 1);
	int changed = 1;
	while (changed)
	{
		changed = 0;
		for (int b = nb - 1; b >= 0; b--)
		{
			IRBlock *blk = &f->blocks[b];
			char *lo = live_out + (size_t)b * bw;
			char *li = live_in + (size_t)b * bw;
			/* live_out = union of successors' live_in. */
			IRInstr *term = blk->count ? &blk->instrs[blk->count - 1] : NULL;
			int succ[2];
			int ns = 0;
			if (term)
			{
				if (term->op == IR_BR)
				{
					succ[ns++] = term->blk_true;
				}
				else if (term->op == IR_BRCOND)
				{
					succ[ns++] = term->blk_true;
					succ[ns++] = term->blk_false;
				}
			}

			for (int v = 0; v < bw; v++)
			{
				char o = 0;
				for (int s = 0; s < ns; s++)
				{
					if (live_in[(size_t)succ[s] * bw + v])
					{
						o = 1;
						break;
					}
				}

				if (o != lo[v])
				{
					lo[v] = o;
					changed = 1;
				}

				/* live_in = gen | (live_out - kill). */
				char ni = gen[(size_t)b * bw + v] || (o && !kill[(size_t)b * bw + v]);
				if (ni != li[v])
				{
					li[v] = ni;
					changed = 1;
				}
			}
		}
	}

	/* Intervals: extend over each block where a value is live, and to every def/use
	   position. A loop-carried value is live across the loop blocks, so its single
	   [start,end] spans the loop. */
	for (int b = 0; b < nb; b++)
	{
		IRBlock *blk = &f->blocks[b];
		int bstart = first_pos[b];
		int bend = first_pos[b] + (blk->count ? blk->count - 1 : 0);
		for (int v = 0; v < bw; v++)
		{
			if (live_in[(size_t)b * bw + v] || live_out[(size_t)b * bw + v])
			{
				if (bstart < a->istart[v]) a->istart[v] = bstart;
				if (bend > a->iend[v]) a->iend[v] = bend;
			}
		}

		for (int i = 0; i < blk->count; i++)
		{
			int p = first_pos[b] + i;
			int def;
			int uses[3];
			int nuse;
			instr_def_use(a, &blk->instrs[i], &def, uses, &nuse);
			if (def >= 0)
			{
				if (p < a->istart[def]) a->istart[def] = p;
				if (p > a->iend[def]) a->iend[def] = p;
			}

			for (int u = 0; u < nuse; u++)
			{
				int uv = uses[u];
				if (uv >= 0)
				{
					if (p < a->istart[uv]) a->istart[uv] = p;
					if (p > a->iend[uv]) a->iend[uv] = p;
				}
			}
		}
	}

	ra_color(f, a, live_out, bw);

	free(first_pos);
	free(gen);
	free(kill);
	free(live_in);
	free(live_out);
	return a;
}
