#ifndef IR_H
#define IR_H

#include "ast.h"   /* TypeKind, Func. */

/* A virtual register id. IR_NO_REG means "no value" (e.g. a store produces none). */
typedef int IRReg;
#define IR_NO_REG (-1)

typedef enum
{
	IR_CONST,     /* dst = imm. */
	IR_MOVE,      /* dst = a. */
	IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
	IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,   /* dst = a <op> b. */
	IR_NEG,       /* dst = -a. */
	IR_CMP,       /* dst = (a <cmp_op> b) ? 1 : 0; cmp_op is a TokenType; type is the operand width. */
	IR_CAST,      /* dst = (to_kind)a; widen/narrow integer or int<->float (a's kind is `type`). */
	IR_LOAD,      /* dst = [base + b*scale + disp]; base is rbp when is_frame, else a. */
	IR_STORE,     /* [base + b*scale + disp] = c; base is rbp when is_frame, else a. */
	IR_BR,        /* Unconditional jump to blk_true. */
	IR_BRCOND,    /* if a != 0 jump blk_true else blk_false. */
	IR_SEL,       /* dst = (a <cmp_op> b) ? c : d - a branchless select (cmov). */
	IR_RET        /* return a (IR_NO_REG for void). */
} IROp;

typedef struct
{
	IROp     op;
	TypeKind type;        /* Result/operand width (the value kind this op computes in). */
	IRReg    dst;         /* Defined vreg, or IR_NO_REG. */
	IRReg    a, b, c;     /* Operand vregs, or IR_NO_REG. */
	IRReg    d;           /* IR_SEL only: the value when the compare is false. */
	long long imm;        /* IR_CONST value. */
	int      scale;       /* IR_LOAD/IR_STORE index scale (1/2/4/8); 0 if no index. */
	long long disp;       /* IR_LOAD/IR_STORE displacement; for a frame local, its slot offset. */
	int      is_frame;    /* IR_LOAD/IR_STORE: base is rbp (a frame local at -disp). */
	int      checked;     /* Non-frame array IR_LOAD/IR_STORE: emit a bounds check (index not BCE-proved safe). */
	int      no_coalesce; /* Split copy (IR_MOVE): the allocator must NOT coalesce this away. */
	int      cmp_op;      /* IR_CMP: a TokenType (TOKEN_LT, ...). */
	TypeKind to_kind;     /* IR_CAST destination kind. */
	int      blk_true;    /* IR_BR/IR_BRCOND target block index. */
	int      blk_false;   /* IR_BRCOND else target block index. */
	int      line;        /* Source line, for diagnostics. */
} IRInstr;

typedef struct
{
	IRInstr *instrs;
	int      count;
	int      cap;
	int      depth;       /* Loop-nesting depth (0 = function top level), for spill weighting. */
} IRBlock;

typedef struct
{
	IRBlock *blocks;
	int      block_count;
	int      block_cap;
	int      vreg_count;     /* Next vreg id to hand out. */
	int      is_region;      /* Loop region (Plan 4): every frame local is potentially
	                            read after the region, so the allocator keeps all
	                            locals live through the region exit. */
	const Func *src;         /* The AST function this was lowered from. */
} IRFunc;

/* Lifecycle. */
IRFunc *ir_func_new(const Func *src);
void    ir_func_free(IRFunc *f);

/* Builders. ir_block_new returns a block index; ir_reg returns a fresh vreg. */
int     ir_block_new(IRFunc *f);
IRReg   ir_reg(IRFunc *f);

/* Append an instruction to block `blk`; returns a pointer valid until the next
   append to the same block (do not retain across appends to that block). */
IRInstr *ir_emit(IRFunc *f, int blk, IROp op, TypeKind type);

#endif
