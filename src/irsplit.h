#ifndef IRSPLIT_H
#define IRSPLIT_H

#include "ir.h"

/* Live-range splitting: split values that pass through a loop unused so the
   allocator can spill the cold span and keep a register free in the loop body.
   A no-op until BZY_IR_SPLIT enables the heuristic (see config). */
void ir_split_func(IRFunc *f);

#endif
