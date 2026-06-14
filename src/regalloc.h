#ifndef REGALLOC_H
#define REGALLOC_H

#include "ir.h"

#define RA_NREGS   11      /* Base allocatable GP registers (rax stays scratch). */
#define RA_MAXREGS 13      /* RA_NREGS + rcx, rdx: claimable when no shift / div-mod needs them. */
#define RA_SPILLED (-1)    /* Not in a register: lives in a frame spill slot. */

#define RA_NXMM    4                       /* Allocatable xmm registers: xmm2..xmm5 (caller-saved on both ABIs). */
#define RA_XMM0    RA_MAXREGS              /* First xmm index in the combined register-index space. */
#define RA_NALL    (RA_MAXREGS + RA_NXMM)  /* Combined index space: GP 0..RA_MAXREGS-1, xmm RA_XMM0..RA_NALL-1. */

/* A value's register class: integer/pointer values live in GP registers, double/
   float values in xmm registers. Derived from the result kind of a value's
   defining instruction. */
typedef enum { RC_GP = 0, RC_XMM = 1 } RegClass;

/* An allocation over "values": the real virtual registers (ids [0, vreg_count))
   plus one pseudo-value per distinct frame local (ids [vreg_count, nval)), so a
   hot local can hold a register across its whole live range. The fields are
   public for the emitter and tests; prefer the accessors below. */
typedef struct
{
	int      *val_reg;       /* [nval] physical register index, or RA_SPILLED. */
	int      *val_slot;      /* [nval] 0-based spill-slot index when spilled (emitter -> rbp offset). */
	int       nval;          /* vreg_count + nlocal. */
	int       vreg_count;    /* Real virtual registers (value ids below this). */
	int       nlocal;        /* Distinct frame locals. */
	long long *local_disp;   /* [nlocal] frame offset of local k (value id vreg_count+k). */
	int       used_reg[RA_NALL];
	int      *val_class;     /* [nval] RegClass per value (RC_GP / RC_XMM). */
	int       spill_bytes;
	int      *istart;        /* [nval] live-interval start position. */
	int      *iend;          /* [nval] live-interval end position. */
	int       hot_spill;       /* 1 if a temporary used in the deepest loop had to spill. */
	int       hot_spill_count; /* How many distinct such temporaries spilled. */
	long long *val_remat;    /* [nval] home-slot disp to rematerialize a spilled value
	                            from (-1 = none): the value is the single-def load of
	                            a local nothing stores, so reading the local's slot at
	                            each use replaces the spill slot and the def's store. */
} IRAlloc;

IRAlloc *ra_run(IRFunc *f);
void     ra_free(IRAlloc *a);

/* Safety gate: 1 if allocating f spills a value used in its deepest loop (a hot
   spill the emitter's tuned heuristics would likely handle better) - the dispatch
   then keeps f on the emitter. Runs an allocation and frees it. */
int      ra_hot_spill(IRFunc *f);

const char *ra_reg_name(int i);
int  ra_is_callee_saved(int i, int linux_target);
int  ra_reg_is_xmm(int i);                       /* 1 if combined index i is an xmm register. */
int  ra_vreg_class(const IRAlloc *a, IRReg v);   /* RC_GP / RC_XMM of a value. */

/* Emitter queries. A register index in [0,RA_NREGS) or RA_SPILLED. */
int  ra_vreg_reg(const IRAlloc *a, IRReg v);
int  ra_vreg_slot(const IRAlloc *a, IRReg v);
long long ra_vreg_remat(const IRAlloc *a, IRReg v);
int  ra_local_reg(const IRAlloc *a, long long disp);
int  ra_local_slot(const IRAlloc *a, long long disp);

/* Debug: the largest live-interval span (loop accumulators span the loop). */
int  ra_debug_max_interval(const IRAlloc *a);

#endif
