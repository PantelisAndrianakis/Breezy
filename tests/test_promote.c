#include "test_framework.h"
#include "parser.h"
#include "types.h"
#include "resolve.h"
#include "prelude.h"
#include "generics.h"   /* Drop uninstantiated generic templates (Pool<T>) before resolve, like the driver. */
#include "ast.h"
#include <string.h>

#define MAX_U 64

/* Parse the prelude + one source string, run the full resolve pipeline (which
   invokes promote_annotate), and return the top-level Func named `fname`. */
static Func *resolve_one(const char *src, const char *fname)
{
	static Parser parsers[MAX_U];
	static Unit *units[MAX_U];
	static TypeTable tt;

	int np = BZY_PRELUDE_COUNT;
	for (int i = 0; i < np; i++)
	{
		parser_init(&parsers[i], BZY_PRELUDE[i]);
		units[i] = parse_unit(&parsers[i]);
	}

	parser_init(&parsers[np], src);
	units[np] = parse_unit(&parsers[np]);
	int total = np + 1;

	{ Unit **gu = units; int gc = MAX_U; generics_expand(&gu, &total, &gc); }   /* Drop uninstantiated templates (Pool<T>), like main.c. */

	types_init(&tt);
	types_register_builtins(&tt);
	for (int i = 0; i < total; i++)
	{
		types_register_unit_names(&tt, units[i]);
	}

	for (int i = 0; i < total; i++)
	{
		types_register_interfaces(&tt, units[i]);
	}

	types_register_all_members(&tt, units, total);

	resolve_program(&tt, units, total);

	for (int i = 0; i < total; i++)
	{
		for (int k = 0; k < units[i]->func_count; k++)
		{
			if (strcmp(units[i]->funcs[k]->name, fname) == 0)
			{
				return units[i]->funcs[k];
			}
		}
	}

	return NULL;
}

/* A loop-carried integer param and a loop-carried local both promote. */
static void test_promotes_loop_scalars(void)
{
	const char *src =
		"long collatzSteps(long n)\n"
		"{\n"
		"	long steps;\n"
		"	steps = 0;\n"
		"	while (n > 1)\n"
		"	{\n"
		"		if (n % 2 == 0) { n = n / 2; } else { n = 3 * n + 1; }\n"
		"		steps = steps + 1;\n"
		"	}\n"
		"	return steps;\n"
		"}\n";
	Func *f = resolve_one(src, "collatzSteps");
	ASSERT(f != NULL);
	ASSERT_INT(f->promo_count, 2);
	/* Distinct registers, drawn from index range 0..3. */
	ASSERT(f->promo_reg[0] != f->promo_reg[1]);
	ASSERT(f->promo_reg[0] >= 0 && f->promo_reg[0] <= 3);
	ASSERT(f->promo_reg[1] >= 0 && f->promo_reg[1] <= 3);
}

/* A function containing a catch handler promotes nothing (unwind hazard gate). */
static void test_try_blocks_promotion(void)
{
	const char *src =
		"long f(long n)\n"
		"{\n"
		"	long acc;\n"
		"	acc = 0;\n"
		"	try { acc = acc + n; } catch (Exception e) { acc = 0; }\n"
		"	return acc;\n"
		"}\n";
	Func *f = resolve_one(src, "f");
	ASSERT(f != NULL);
	ASSERT_INT(f->promo_count, 0);
}

/* No more than four locals are ever promoted. */
static void test_caps_at_four(void)
{
	const char *src =
		"long g(long a)\n"
		"{\n"
		"	long b; long c; long d; long e; long h;\n"
		"	b = a; c = a; d = a; e = a; h = a;\n"
		"	while (a > 0) { b = b + 1; c = c + 1; d = d + 1; e = e + 1; h = h + 1; a = a - 1; }\n"
		"	return b + c + d + e + h;\n"
		"}\n";
	Func *f = resolve_one(src, "g");
	ASSERT(f != NULL);
	ASSERT(f->promo_count <= 4);
}

int main(void)
{
	RUN(test_promotes_loop_scalars);
	RUN(test_try_blocks_promotion);
	RUN(test_caps_at_four);
	return 0;
}
