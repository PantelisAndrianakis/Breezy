#include "regalloc.h"
#include <stdlib.h>
#include <string.h>

/* Allocatable GP registers. rax/rcx/rdx are deliberately absent: they stay
   scratch for idiv (rax/rdx), shift counts (cl), and spill reloads. */
/* Indices 0..RA_NREGS-1 are the base pool; 11=rcx and 12=rdx are claimable per
   function when no variable shift / div-mod needs them as fixed scratch. */
static const char *RA_REGS[RA_MAXREGS] =
	{ "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rcx", "rdx" };

/* Allocatable xmm registers, indexed by (combined index - RA_XMM0). xmm0/xmm1 stay
   scratch (immediate computation / spill staging); xmm2..xmm5 are caller-saved on
   both Win64 and System V, matching the legacy emitter's float-promotion pool. */
static const char *RA_XMM_REGS[RA_NXMM] = { "xmm2", "xmm3", "xmm4", "xmm5" };

const char *ra_reg_name(int i)
{
	if (i >= RA_XMM0)
	{
		return RA_XMM_REGS[i - RA_XMM0];
	}

	return RA_REGS[i];
}

int ra_reg_is_xmm(int i)
{
	return i >= RA_XMM0;
}

int ra_is_callee_saved(int i, int linux_target)
{
	if (i >= RA_XMM0)
	{
		return 0;   /* xmm2..xmm5 caller-saved on both ABIs. */
	}

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
   and reads the stored vreg; everything else uses a/b/c (and IR_SEL's d) and
   defines dst. */
static void instr_def_use(const IRAlloc *a, const IRInstr *in, int *def, int uses[4], int *nuse)
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

	if (in->op == IR_SEL && in->d != IR_NO_REG)
	{
		uses[(*nuse)++] = in->d;
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

long long ra_vreg_remat(const IRAlloc *a, IRReg v)
{
	return a->val_remat[v];
}

int ra_vreg_class(const IRAlloc *a, IRReg v)
{
	return a->val_class[v];
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
	free(a->val_remat);
	free(a->val_class);
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

/* ---- register class ------------------------------------------------------- */

static int ra_is_float_kind(TypeKind k)
{
	return k == TY_DOUBLE || k == TY_FLOAT;
}

/* The kind of data a value HOLDS (its register-class driver). For most ops that is
   the instruction's result width; a compare yields an integer 0/1 regardless of its
   operand width; a cast yields its destination kind. */
static int ra_result_is_float(const IRInstr *in)
{
	if (in->op == IR_CMP)
	{
		return 0;
	}

	if (in->op == IR_CAST)
	{
		return ra_is_float_kind(in->to_kind);
	}

	return ra_is_float_kind(in->type);
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
	char *used_deep = calloc((size_t)nval, 1);   /* Touched in a maximum-depth block. */

	int maxdepth = 0;
	for (int b = 0; b < f->block_count; b++)
	{
		if (f->blocks[b].depth > maxdepth)
		{
			maxdepth = f->blocks[b].depth;
		}
	}

	/* Build interference + copy lists from per-point liveness, and accumulate each
	   value's loop-depth-weighted use/def count. */
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		long long fac = depth_weight(blk->depth);
		int deepest = (blk->depth == maxdepth);
		memcpy(live, live_out + (size_t)b * bw, (size_t)nval);
		for (int i = blk->count - 1; i >= 0; i--)
		{
			IRInstr *in = &blk->instrs[i];
			int def;
			int uses[4];
			int nuse;
			instr_def_use(a, in, &def, uses, &nuse);
			if (def >= 0)
			{
				weight[def] += fac;
				if (deepest)
				{
					used_deep[def] = 1;
				}
			}

			for (int u = 0; u < nuse; u++)
			{
				if (uses[u] >= 0)
				{
					weight[uses[u]] += fac;
					if (deepest)
					{
						used_deep[uses[u]] = 1;
					}
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

	/* Claim rcx (11) / rdx (12) as extra allocatable registers when no variable
	   shift / div-mod needs them as fixed scratch. They are only safe to hand to
	   values when the colouring does not spill (a spill reload stages operands
	   through rax/rcx/rdx); so colour with the extras, and if that still spills,
	   recolour with the base 11 so the extras revert to scratch. */
	int has_divmod = 0;
	int has_shift = 0;
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IROp op = blk->instrs[i].op;
			if (op == IR_DIV || op == IR_MOD)
			{
				has_divmod = 1;
			}
			else if (op == IR_SHL || op == IR_SHR)
			{
				has_shift = 1;
			}
		}
	}

	char allow[RA_NALL];
	for (int r = 0; r < RA_NALL; r++)
	{
		allow[r] = (r < RA_NREGS) ? 1 : 0;
	}

	allow[11] = !has_shift;    /* rcx. */
	allow[12] = !has_divmod;   /* rdx. */
	for (int r = RA_XMM0; r < RA_NALL; r++)
	{
		allow[r] = 1;          /* xmm2..xmm5 always allocatable (no fixed-scratch role). */
	}

	int *color = malloc((size_t)nval * sizeof(int));
	for (int attempt = 0; attempt < 2; attempt++)
	{
		for (int r = 0; r < nval; r++)
		{
			color[r] = -1;
		}

		for (int i = 0; i < no; i++)
		{
			int r = order[i];
			char used[RA_NALL];
			memset(used, 0, sizeof used);
			for (int k = 0; k < i; k++)
			{
				int r2 = order[k];
				if (color[r2] >= 0 && rintf[(size_t)r * nval + r2])
				{
					used[color[r2]] = 1;
				}
			}

			/* Restrict the search to the root's register class. A coalesced root's
			   members are copy-related and so share a class; colour from that
			   class's contiguous index sub-range. */
			int lo = (a->val_class[r] == RC_XMM) ? RA_XMM0 : 0;
			int hi = (a->val_class[r] == RC_XMM) ? RA_NALL : RA_MAXREGS;
			for (int rr = lo; rr < hi; rr++)
			{
				if (allow[rr] && !used[rr])
				{
					color[r] = rr;
					break;
				}
			}
		}

		int spilled = 0;
		for (int i = 0; i < no; i++)
		{
			if (color[order[i]] < 0)
			{
				spilled = 1;
				break;
			}
		}

		if (!spilled || (!allow[11] && !allow[12]))
		{
			break;   /* Fits, or already on the base pool - nothing more to try. */
		}

		allow[11] = 0;   /* Spilled with the extras: recolour without them so they */
		allow[12] = 0;   /* stay available as spill-reload scratch. */
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

	/* Rematerializable spills: a spilled vreg defined exactly once, by a frame
	   load of a local nothing in the function stores, reloads from the local's
	   home slot at each use - the reload costs what the spill-slot read would,
	   the def's slot store disappears, and no spill slot is owned. Exempt from
	   the hot-spill gate: a remat read is no worse than the register pressure
	   the emitter fallback would face. */
	{
		int *defs = calloc((size_t)nval, sizeof(int));
		long long *def_disp = malloc((size_t)nval * sizeof(long long));
		int *def_blk = malloc((size_t)nval * sizeof(int));
		int *def_idx = malloc((size_t)nval * sizeof(int));
		char *lstored = calloc((size_t)(a->nlocal > 0 ? a->nlocal : 1), 1);
		for (int v = 0; v < nval; v++)
		{
			def_disp[v] = -1;
		}

		/* Linear position of each block's first instruction: the same scheme
		   liveness used, so positions are comparable with istart/iend. */
		int *bfirst = malloc((size_t)(f->block_count > 0 ? f->block_count : 1) * sizeof(int));
		int pos = 0;
		for (int b = 0; b < f->block_count; b++)
		{
			bfirst[b] = pos;
			pos += f->blocks[b].count;
		}

		for (int b = 0; b < f->block_count; b++)
		{
			for (int i = 0; i < f->blocks[b].count; i++)
			{
				IRInstr *in = &f->blocks[b].instrs[i];
				if (in->dst >= 0)
				{
					defs[in->dst]++;
					def_disp[in->dst] = (in->op == IR_LOAD && in->is_frame) ? in->disp : -1;
					def_blk[in->dst] = b;
					def_idx[in->dst] = i;
				}

				if (in->op == IR_STORE && in->is_frame)
				{
					int k = local_index(a, in->disp);
					if (k >= 0)
					{
						lstored[k] = 1;
					}
				}
			}
		}

		for (int v = 0; v < a->vreg_count; v++)
		{
			if (a->val_reg[v] != RA_SPILLED || a->iend[v] < 0
				|| defs[v] != 1 || def_disp[v] < 0)
			{
				continue;
			}

			if (!lstored[local_index(a, def_disp[v])])
			{
				a->val_remat[v] = def_disp[v];
				continue;
			}

			/* The local IS stored somewhere, but a load whose uses all sit in
			   the def's own block is still safe when no store to that local
			   lies strictly between the def and the last use: a basic block
			   executes linearly, so every reload observes the defining value.
			   (A store AT the last use reads its operands first, so it is
			   harmless.) */
			int b = def_blk[v];
			IRBlock *blk = &f->blocks[b];
			if (a->iend[v] >= bfirst[b] + blk->count)
			{
				continue;   /* Live past the block: path order is not linear. */
			}

			int lastu = a->iend[v] - bfirst[b];
			int clean = 1;
			for (int i = def_idx[v] + 1; i < lastu; i++)
			{
				IRInstr *in = &blk->instrs[i];
				if (in->op == IR_STORE && in->is_frame && in->disp == def_disp[v])
				{
					clean = 0;
					break;
				}
			}

			if (clean)
			{
				a->val_remat[v] = def_disp[v];
			}
		}

		/* A spilled local pseudo-value of a never-stored local is equally
		   harmless: its reads already go to the home slot (no spill slot, no
		   extra traffic), so it neither owns a slot nor counts as hot. */
		for (int k = 0; k < a->nlocal; k++)
		{
			if (!lstored[k] && a->val_reg[a->vreg_count + k] == RA_SPILLED
				&& a->iend[a->vreg_count + k] >= 0)
			{
				a->val_remat[a->vreg_count + k] = a->local_disp[k];
			}
		}

		free(defs);
		free(def_disp);
		free(def_blk);
		free(def_idx);
		free(bfirst);
		free(lstored);
	}

	/* One spill slot per spilled root, shared by its coalesced members. A
	   rematerializing member does not force a slot (it reads the local's home
	   slot); a non-remat member of the same class still gets one. */
	int *root_slot = malloc((size_t)nval * sizeof(int));
	for (int r = 0; r < nval; r++)
	{
		root_slot[r] = -1;
	}

	int nspill = 0;
	for (int v = 0; v < nval; v++)
	{
		if (a->val_reg[v] == RA_SPILLED && a->iend[v] >= 0 && a->val_remat[v] < 0)
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

	/* Hot spill: TEMPORARIES used in the function's deepest loop had to spill,
	   in numbers the region's other gains cannot amortize. Only meaningful when
	   there is a loop (maxdepth >= 1). Rematerializing values are exempt (no
	   slot, def-store gone), and so are local pseudo-values: a spilled local
	   simply lives in its home slot, which is no worse than the emitter
	   fallback's own treatment of locals. The tolerance is empirical: the
	   packet pipeline's region carries 14 spilled temps and still beats the
	   emitter 2x. The bound was 16 when high-spill regions also paid a per-copy
	   imul tax the emitter folded away; once IR lowering folds that constant
	   index/stride arithmetic (low_const), the codec block sweep WINS at 108
	   spilled temps (240 vs 271 ms, both OSes, full-suite A/B), so the bound is
	   raised to 128. codec is the only region in the suite above 16; the A/B
	   confirmed no other kernel's routing changes. */
	a->hot_spill = 0;
	a->hot_spill_count = 0;
	if (maxdepth >= 1)
	{
		int xmm_hot = 0;
		for (int v = 0; v < a->vreg_count; v++)
		{
			if (a->val_reg[v] == RA_SPILLED && a->iend[v] >= 0 && used_deep[v]
				&& a->val_remat[v] < 0)
			{
				a->hot_spill_count++;
				if (a->val_class[v] == RC_XMM)
				{
					xmm_hot++;
				}
			}
		}

		/* GP spills tolerate a high count (the IR's 11-register pool plus folded
		   addressing still beats the emitter - e.g. codec at 108). FP is different:
		   the IR has only 4 allocatable xmm registers (xmm2-5) while the emitter's
		   float promotion uses xmm2-11, so any xmm spill in the hottest loop means
		   the emitter wins - fall back. (Until packed vectorization, which is what
		   actually beats the emitter on FP.) */
		a->hot_spill = (a->hot_spill_count > 128) || (xmm_hot > 0);
	}

	free(interf);
	free(cp_d);
	free(cp_s);
	free(live);
	free(weight);
	free(used_deep);
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
	a->val_remat = malloc((size_t)nval * sizeof(long long));
	for (int v = 0; v < nval; v++)
	{
		a->val_reg[v] = RA_SPILLED;
		a->istart[v] = 0x7fffffff;
		a->iend[v] = -1;
		a->val_remat[v] = -1;
	}

	/* Register class per value (RC_GP default). A value's class follows the result
	   kind of its defining instruction; a frame local's class follows the kind of
	   the loads/stores that touch it. (While float eligibility is off, every value
	   is integer/RC_GP and the xmm pool is never selected.) */
	a->val_class = calloc((size_t)nval, sizeof(int));
	for (int b = 0; b < f->block_count; b++)
	{
		IRBlock *blk = &f->blocks[b];
		for (int i = 0; i < blk->count; i++)
		{
			IRInstr *in = &blk->instrs[i];
			if (in->op == IR_STORE && in->is_frame)
			{
				int k = local_index(a, in->disp);
				if (k >= 0 && ra_is_float_kind(in->type))
				{
					a->val_class[a->vreg_count + k] = RC_XMM;
				}
			}
			else if (in->op == IR_LOAD && in->is_frame)
			{
				int k = local_index(a, in->disp);
				if (k >= 0 && ra_is_float_kind(in->type))
				{
					a->val_class[a->vreg_count + k] = RC_XMM;
				}

				if (in->dst >= 0 && ra_is_float_kind(in->type))
				{
					a->val_class[in->dst] = RC_XMM;
				}
			}
			else if (in->dst >= 0 && ra_result_is_float(in))
			{
				a->val_class[in->dst] = RC_XMM;
			}
		}
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
			int uses[4];
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
				/* Region exit (the only successor-less block in a region): every
				   frame local is potentially read after the region, so all local
				   pseudo-values are live-out there. This keeps two live-out locals
				   from ever sharing a register and keeps a local's updated value in
				   its register until the region's store-back. */
				char o = (f->is_region && ns == 0 && v >= a->vreg_count) ? 1 : 0;
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
			int uses[4];
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

int ra_hot_spill(IRFunc *f)
{
	IRAlloc *a = ra_run(f);
	int hot = a->hot_spill;
	ra_free(a);
	return hot;
}
