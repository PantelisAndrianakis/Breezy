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

/* ---- linear scan ---------------------------------------------------------- */

typedef struct
{
	int val;
	int start;
	int end;
} RaIv;

static int raiv_cmp(const void *x, const void *y)
{
	const RaIv *p = x;
	const RaIv *q = y;
	if (p->start != q->start)
	{
		return p->start - q->start;
	}

	return p->end - q->end;
}

/* Classic linear scan: walk intervals by start; expire those that ended; assign a
   free register, else spill whichever of the current/active intervals ends later.
   Spilled values get a fresh 0-based slot index (the emitter turns it into an rbp
   offset). At most RA_NREGS intervals hold registers at once. */
static void ra_linscan(IRAlloc *a)
{
	int nval = a->nval;
	RaIv *iv = malloc((size_t)(nval > 0 ? nval : 1) * sizeof(RaIv));
	int niv = 0;
	for (int v = 0; v < nval; v++)
	{
		if (a->iend[v] >= 0)
		{
			iv[niv].val = v;
			iv[niv].start = a->istart[v];
			iv[niv].end = a->iend[v];
			niv++;
		}
	}

	qsort(iv, (size_t)niv, sizeof(RaIv), raiv_cmp);

	int act_val[RA_NREGS];
	int act_end[RA_NREGS];
	int act_reg[RA_NREGS];
	int nact = 0;
	int regfree[RA_NREGS];
	for (int r = 0; r < RA_NREGS; r++)
	{
		regfree[r] = 1;
	}

	int nspill = 0;

	for (int i = 0; i < niv; i++)
	{
		int start = iv[i].start;

		/* Expire intervals that ended before this one begins. */
		int w = 0;
		for (int j = 0; j < nact; j++)
		{
			if (act_end[j] < start)
			{
				regfree[act_reg[j]] = 1;
			}
			else
			{
				act_val[w] = act_val[j];
				act_end[w] = act_end[j];
				act_reg[w] = act_reg[j];
				w++;
			}
		}

		nact = w;

		int reg = -1;
		for (int r = 0; r < RA_NREGS; r++)
		{
			if (regfree[r])
			{
				reg = r;
				break;
			}
		}

		if (reg >= 0)
		{
			regfree[reg] = 0;
			a->val_reg[iv[i].val] = reg;
			a->used_reg[reg] = 1;
			act_val[nact] = iv[i].val;
			act_end[nact] = iv[i].end;
			act_reg[nact] = reg;
			nact++;
		}
		else
		{
			/* No register free: spill the interval that ends furthest out. */
			int sp = 0;
			for (int j = 1; j < nact; j++)
			{
				if (act_end[j] > act_end[sp])
				{
					sp = j;
				}
			}

			if (act_end[sp] > iv[i].end)
			{
				int r = act_reg[sp];
				a->val_reg[act_val[sp]] = RA_SPILLED;
				a->val_slot[act_val[sp]] = nspill++;
				a->val_reg[iv[i].val] = r;
				a->used_reg[r] = 1;
				act_val[sp] = iv[i].val;
				act_end[sp] = iv[i].end;
			}
			else
			{
				a->val_reg[iv[i].val] = RA_SPILLED;
				a->val_slot[iv[i].val] = nspill++;
			}
		}
	}

	a->spill_bytes = nspill * 8;
	free(iv);
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

	ra_linscan(a);

	free(first_pos);
	free(gen);
	free(kill);
	free(live_in);
	free(live_out);
	return a;
}
