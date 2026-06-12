#include "overload.h"
#include <string.h>
#include <stdio.h>

/* A single, unique code per scalar TypeKind; composite kinds recurse. The map
   is total and injective over TypeKind so two different parameter lists can
   never produce the same suffix. The rare handle kinds get a "Z<n>" code keyed
   on the enum value — still injective, and they never appear as overload
   discriminators in practice. */
static void encode_one(char *out, size_t cap, int *n, const TypeRef *t)
{
	#define PUT(ch) do { if (*n + 1 < (int)cap) { out[(*n)++] = (char)(ch); } } while (0)
	#define PUTS(s) do { const char *_p = (s); while (*_p) { PUT(*_p++); } } while (0)
	char buf[80];
	switch (t->kind)
	{
	case TY_BOOL:   PUT('b'); break;
	case TY_BYTE:   PUT('c'); break;
	case TY_UBYTE:  PUT('C'); break;
	case TY_SHORT:  PUT('h'); break;
	case TY_USHORT: PUT('H'); break;
	case TY_INT:    PUT('i'); break;
	case TY_UINT:   PUT('I'); break;
	case TY_LONG:   PUT('l'); break;
	case TY_ULONG:  PUT('L'); break;
	case TY_FLOAT:  PUT('f'); break;
	case TY_DOUBLE: PUT('d'); break;
	case TY_STRING: PUT('S'); break;
	case TY_VOID:   PUT('v'); break;
	case TY_NULL:   PUT('n'); break;
	case TY_OBJECT:
		snprintf(buf, sizeof(buf), "O%d%s", (int)strlen(t->class_name), t->class_name);
		PUTS(buf);
		break;
	case TY_ARRAY:
		PUT('A');
		encode_one(out, cap, n, t->elem);
		break;
	case TY_MAP:
		PUT('M');
		encode_one(out, cap, n, t->elem);
		encode_one(out, cap, n, t->elem2);
		break;
	case TY_ENTRY:
		PUT('E');
		if (t->elem)  { encode_one(out, cap, n, t->elem); }
		if (t->elem2) { encode_one(out, cap, n, t->elem2); }
		break;
	case TY_GENERIC:
		snprintf(buf, sizeof(buf), "G%d%s", (int)strlen(t->class_name), t->class_name);
		PUTS(buf);
		if (t->targ_count > 0)
		{
			for (int i = 0; i < t->targ_count; i++)
			{
				encode_one(out, cap, n, t->targs[i]);
			}
		}
		else if (t->elem)
		{
			encode_one(out, cap, n, t->elem);
		}
		break;
	case TY_CHANNEL:
		PUT('N');
		if (t->elem) { encode_one(out, cap, n, t->elem); }
		break;
	default:
		/* Handle kinds (Timer/Socket/...): injective "Z<enum>". */
		snprintf(buf, sizeof(buf), "Z%d", (int)t->kind);
		PUTS(buf);
		break;
	}
	#undef PUTS
	#undef PUT
}

void overload_encode_types(char *out, size_t cap, const TypeRef *types, int count)
{
	if (cap == 0)
	{
		return;
	}

	if (count == 0)
	{
		snprintf(out, cap, "v");
		return;
	}

	int n = 0;
	for (int i = 0; i < count; i++)
	{
		encode_one(out, cap, &n, &types[i]);
	}
	out[n < (int)cap ? n : (int)cap - 1] = '\0';
}

/* Exact element/class equality for objects, arrays, maps, generics. */
static int typeref_same(const TypeRef *a, const TypeRef *b)
{
	if (a->kind != b->kind)
	{
		return 0;
	}

	if (a->kind == TY_OBJECT)
	{
		return strcmp(a->class_name, b->class_name) == 0;
	}

	if (a->kind == TY_ARRAY)
	{
		return a->elem && b->elem && typeref_same(a->elem, b->elem);
	}

	if (a->kind == TY_MAP)
	{
		return a->elem && b->elem && a->elem2 && b->elem2
			   && typeref_same(a->elem, b->elem) && typeref_same(a->elem2, b->elem2);
	}

	if (a->kind == TY_GENERIC)
	{
		return strcmp(a->class_name, b->class_name) == 0
			   && a->elem && b->elem && typeref_same(a->elem, b->elem);
	}

	return 1;   /* Same scalar/bool/string kind. */
}

int overload_rank_arg(const TypeRef *param, const TypeRef *arg)
{
	if (arg->kind == TY_NULL)
	{
		return ty_is_managed(param->kind) ? 2 : -1;
	}

	if (typeref_same(param, arg))
	{
		return 0;
	}

	/* Widening: the same conversions assignable() allows. */
	if (ty_is_int(param->kind) && ty_is_int(arg->kind)
			&& ty_is_signed(param->kind) == ty_is_signed(arg->kind)
			&& ty_rank(arg->kind) <= ty_rank(param->kind))
	{
		return 1;
	}

	if (param->kind == TY_DOUBLE && ty_is_int(arg->kind))
	{
		return 1;
	}

	/* Permissive: any object assigns to any object slot (assignable() rule). */
	if (param->kind == TY_OBJECT && arg->kind == TY_OBJECT)
	{
		return 2;
	}

	return -1;
}

/* Rank every argument of one candidate; fill ranks[], return 1 if viable. */
static int rank_candidate(const OverloadCand *c, const TypeRef *args, int argc, int *ranks)
{
	if (argc < c->min_args || argc > c->param_count)
	{
		return 0;
	}

	for (int i = 0; i < argc; i++)
	{
		int r = overload_rank_arg(&c->param_types[i], &args[i]);
		if (r < 0)
		{
			return 0;
		}

		ranks[i] = r;
	}

	return 1;
}

int overload_select(const OverloadCand *cands, int ncand, const TypeRef *args, int argc)
{
	int viable[64], nv = 0;
	int ranks[64][8];
	for (int c = 0; c < ncand && c < 64; c++)
	{
		if (rank_candidate(&cands[c], args, argc, ranks[nv]))
		{
			viable[nv++] = c;
		}
	}

	if (nv == 0)
	{
		return OVL_NONE;
	}

	if (nv == 1)
	{
		return viable[0];
	}

	/* Best = dominates every other: no worse on all args, strictly better on at
	   least one. Lower rank is better. */
	for (int a = 0; a < nv; a++)
	{
		int dominates_all = 1;
		for (int b = 0; b < nv && dominates_all; b++)
		{
			if (a == b)
			{
				continue;
			}

			int no_worse = 1, strictly_better = 0;
			for (int i = 0; i < argc; i++)
			{
				if (ranks[a][i] > ranks[b][i])
				{
					no_worse = 0;
				}

				if (ranks[a][i] < ranks[b][i])
				{
					strictly_better = 1;
				}
			}

			if (!(no_worse && strictly_better))
			{
				dominates_all = 0;
			}
		}

		if (dominates_all)
		{
			return viable[a];
		}
	}

	return OVL_AMBIG;
}

int overload_min_args(const Func *f)
{
	for (int i = 0; i < f->param_count; i++)
	{
		if (f->params[i].def != NULL)
		{
			return i;
		}
	}

	return f->param_count;
}

int overload_set_is_ambiguous(const OverloadCand *cands, int ncand)
{
	/* Probe each candidate's own parameter prefix (an exact, rank-0 match for
	   that candidate) at every arity it admits. If the full set ties on that
	   probe, two candidates are mutually indistinguishable for that arity. */
	for (int c = 0; c < ncand; c++)
	{
		for (int n = cands[c].min_args; n <= cands[c].param_count; n++)
		{
			if (overload_select(cands, ncand, cands[c].param_types, n) == OVL_AMBIG)
			{
				return 1;
			}
		}
	}

	return 0;
}
