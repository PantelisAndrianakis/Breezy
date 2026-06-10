#include "ir.h"
#include <stdlib.h>
#include <string.h>

IRFunc *ir_func_new(const Func *src)
{
	IRFunc *f = calloc(1, sizeof(IRFunc));
	f->src = src;
	return f;
}

void ir_func_free(IRFunc *f)
{
	if (!f)
	{
		return;
	}

	for (int i = 0; i < f->block_count; i++)
	{
		free(f->blocks[i].instrs);
	}

	free(f->blocks);
	free(f);
}

int ir_block_new(IRFunc *f)
{
	if (f->block_count == f->block_cap)
	{
		f->block_cap = f->block_cap ? f->block_cap * 2 : 8;
		f->blocks = realloc(f->blocks, (size_t)f->block_cap * sizeof(IRBlock));
	}

	IRBlock *b = &f->blocks[f->block_count];
	memset(b, 0, sizeof(*b));
	return f->block_count++;
}

IRReg ir_reg(IRFunc *f)
{
	return f->vreg_count++;
}

IRInstr *ir_emit(IRFunc *f, int blk, IROp op, TypeKind type)
{
	IRBlock *b = &f->blocks[blk];
	if (b->count == b->cap)
	{
		b->cap = b->cap ? b->cap * 2 : 8;
		b->instrs = realloc(b->instrs, (size_t)b->cap * sizeof(IRInstr));
	}

	IRInstr *in = &b->instrs[b->count++];
	memset(in, 0, sizeof(*in));
	in->op = op;
	in->type = type;
	in->dst = IR_NO_REG;
	in->a = IR_NO_REG;
	in->b = IR_NO_REG;
	in->c = IR_NO_REG;
	in->d = IR_NO_REG;
	in->scale = 0;
	in->is_frame = 0;
	in->blk_true = -1;
	in->blk_false = -1;
	return in;
}
