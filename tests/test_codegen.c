#include "test_framework.h"
#include "parser.h"
#include "types.h"
#include "resolve.h"
#include "codegen.h"
#include "prelude.h"

#define MAX_U 64
static char g_asm[1 << 16];

/* Compile `nsrc` source strings for `target` (prelude first, then all srcs)
   and load the emitted asm into g_asm. */
static void emit_n(const char **srcs, int nsrc, Target target)
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

	for (int j = 0; j < nsrc; j++)
	{
		parser_init(&parsers[np + j], srcs[j]);
		units[np + j] = parse_unit(&parsers[np + j]);
	}
	int total = np + nsrc;

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
	for (int i = 0; i < total; i++)
	{
		types_register_unit_members(&tt, units[i]);
	}
	resolve_program(&tt, units, total);

	/* A real file in the cwd (MinGW's tmpfile() targets C:\ and may return NULL). */
	FILE *f = fopen("out_cg_test.asm", "w+");
	if (!f)
	{
		printf("FAIL\n    cannot open out_cg_test.asm\n");
		exit(1);
	}

	Codegen cg;
	cg_init(&cg, f);
	cg.target = target;
	cg_program(&cg, &tt, units, total);
	fflush(f);
	rewind(f);
	size_t n = fread(g_asm, 1, sizeof(g_asm) - 1, f);
	g_asm[n] = '\0';
	fclose(f);
}

/* Convenience wrapper: compile a single source string (prelude + the user unit,
   exactly as the driver does) and load the emitted asm into g_asm. */
static void emit(const char *src, Target target)
{
	const char *srcs[1] = { src };
	emit_n(srcs, 1, target);
}

static void test_linux_arg_regs(void)
{
	/* add(a,b) called as add(2,3): on SysV the two int args load into rdi/rsi. */
	emit("int add(int a, int b) { return a + b; } void main() { int r; r = add(2, 3); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "rdi") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "rsi") != NULL, 1);
}

static void test_windows_arg_regs_unchanged(void)
{
	emit("int add(int a, int b) { return a + b; } void main() { int r; r = add(2, 3); }", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "rcx") != NULL, 1);   /* arg0 still rcx on Win64. */
	ASSERT_INT(strstr(g_asm, "rdi") == NULL, 1);   /* never SysV regs on Windows. */
}

static void test_linux_receiver_and_release(void)
{
	/* A method call + an object local: the receiver passes in rdi (SysV arg0). */
	emit("class A { int v; int get() { return this.v; } }"
		 " void main() { A a; a = new A(); int x; x = a.get(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "rdi") != NULL, 1);
}

static void test_linux_fp_arg_numbering(void)
{
	/* f(int a, double b): a -> rdi, b -> xmm0 (FP numbered independently of the
	   integer arg in position 0). */
	emit("double f(int a, double b) { return b; } void main() { double r; r = f(1, 2.5); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "rdi") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "xmm0") != NULL, 1);
}

static void test_static_rsp_no_push_no_realign(void)
{
	/* The fixed-rsp frame keeps rsp static across the whole body: expression
	   temporaries live in frame slots (no push rax) and calls need no per-call
	   realign (no and rsp, -16). This function does arithmetic (temp preservation)
	   and allocates an escaping object (a runtime call via cg_aligned_call). */
	emit("class A { int v; } A make() { return new A(); }"
		 " void main() { A a; a = make(); int x; x = (1 + 2) + (3 + 4); }", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "push rax") == NULL, 1);     /* No expression pushes remain. */
	ASSERT_INT(strstr(g_asm, "and rsp, -16") == NULL, 1); /* No per-call realign remains. */
}

static void test_devirt_monomorphic_direct(void)
{
	/* A leaf class with no subclass: the call site is monomorphic, so dispatch
	   must be a direct `call A__get` with no vtable indirection (`call r11`). */
	emit("class A { int v; int get() { return this.v; } }"
		 " void main() { A a; a = new A(); int x; x = a.get(); }", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "call A__get") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "call r11") == NULL, 1);
}

static void test_devirt_polymorphic_stays_indirect(void)
{
	/* Base method overridden by a subclass: a base-typed receiver might hold the
	   subclass, so dispatch MUST stay indirect (`call r11`). */
	const char *srcs[] =
	{
		"class Animal { void speak() { print(0); } }",
		"class Dog extends Animal { void speak() { print(1); } }",
		"void main() { Animal a; a = new Dog(); a.speak(); }"
	};
	emit_n(srcs, 3, TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "call r11") != NULL, 1);
}

static void test_devirt_unoverridden_base_method(void)
{
	/* Animal has a subclass (Dog), but `tag` is never overridden. A call to
	   tag() on an Animal-typed receiver is still monomorphic -> direct call. */
	const char *srcs[] =
	{
		"class Animal { int tag() { return 7; } void speak() { print(0); } }",
		"class Dog extends Animal { void speak() { print(1); } }",
		"void main() { Animal a; a = new Dog(); int t; t = a.tag(); }"
	};
	emit_n(srcs, 3, TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "call Animal__tag") != NULL, 1);
}

static void test_bitwise_emission(void)
{
	emit("void main() { int a; a = 6 & 3; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "and rax, rbx") != NULL, 1);

	emit("void main() { int a; a = 6 | 1; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "or rax, rbx") != NULL, 1);

	emit("void main() { int a; a = 5 ^ 1; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "xor rax, rbx") != NULL, 1);

	emit("void main() { int a; a = ~0; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "not rax") != NULL, 1);
}

static void test_logical_emission(void)
{
	/* Short-circuit and/or branch on the lhs (cmp + conditional jump). */
	emit("bool f(bool a, bool b) { return a and b; } void main() { }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "je .L") != NULL, 1);

	emit("bool g(bool a, bool b) { return a or b; } void main() { }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "jne .L") != NULL, 1);

	/* xor evaluates both and xors the 0/1 values. */
	emit("bool h(bool a, bool b) { return a xor b; } void main() { }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "xor rax, rbx") != NULL, 1);

	/* not negates: cmp + sete. */
	emit("bool n(bool a) { return not a; } void main() { }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "sete al") != NULL, 1);
}

static void test_div_strength_reduction(void)
{
	/* '/' by a power-of-two literal becomes an (arithmetic) shift, not idiv. */
	emit("void main() { int n; n = 100; int a; a = n / 2; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "idiv") == NULL, 1);
	ASSERT_INT(strstr(g_asm, "sar rax, 1") != NULL, 1);

	/* '%' by a power-of-two literal becomes a mask, not idiv. */
	emit("void main() { int n; n = 100; int a; a = n % 2; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "idiv") == NULL, 1);
	ASSERT_INT(strstr(g_asm, "and rax, 1") != NULL, 1);

	/* Unsigned divide uses a logical shift (no sign bias). */
	emit("void main() { uint n; n = 100u; uint a; a = n / 4; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "idiv") == NULL, 1);
	ASSERT_INT(strstr(g_asm, "shr rax, 2") != NULL, 1);

	/* A non-power-of-two constant divisor still uses idiv (general path). */
	emit("void main() { int n; n = 100; int a; a = n / 3; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "idiv") != NULL, 1);
}

static void test_branch_fusion(void)
{
	/* A comparison condition lowers to a cmp + inverted conditional jump rather
	   than materializing a 0/1 boolean and testing it. The two-line forms below are
	   specific to the fused output ('n > 1' -> jump-unless = jle; 'n < 3' -> jge);
	   plain instruction names like setg/jle also appear in the prelude, so the test
	   asserts the exact fused pair instead. */
	emit("void main() { int n; n = 5; if (n > 1) { print(1); } }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "cmp rax, 1\n    jle") != NULL, 1);

	emit("void main() { int n; n = 5; if (n < 3) { print(1); } }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "cmp rax, 3\n    jge") != NULL, 1);
}

static void test_divisibility_test(void)
{
	/* (X % 2^k) == 0 / != 0 in a condition collapses to a single mask that sets ZF -
	   no idiv, no signed-remainder reconstruction. == 0 inverts to jne, != 0 to je. */
	emit("void main() { int n; n = 5; if (n % 2 == 0) { print(1); } }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "and rax, 1\n    jne") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "idiv") == NULL, 1);

	emit("void main() { int n; n = 5; if (n % 8 != 0) { print(1); } }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "and rax, 7\n    je") != NULL, 1);
}

static void test_promotion_register_resident(void)
{
	/* A hot 64-bit param (n) and loop-carried local (steps) are promoted to
	   r12/r13: n is seeded from its arg register, r12 is saved in the prologue,
	   and the loop reads n from the register instead of its [rbp-8] slot. */
	emit(
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
		"}\n"
		"void main() { print(collatzSteps(7)); }\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "mov r12, rcx") != NULL, 1);   /* param n seeded into r12 (not spilled). */
	ASSERT_INT(strstr(g_asm, "], r12") != NULL, 1);         /* prologue saves the caller's r12. */
	ASSERT_INT(strstr(g_asm, "mov rax, r12") != NULL, 1);   /* n is read from the register. */
}

static void test_inplace_register_arithmetic(void)
{
	/* state = state * C1 + C2 updates r13 in place (no `mov r13, rax`), and
	   i = i + 1 becomes a single `add`. */
	emit(
		"long lcg(long iters)\n"
		"{\n"
		"	long state; state = 1;\n"
		"	for (long i = 0; i < iters; i = i + 1)\n"
		"	{\n"
		"		state = state * 6364136223846793005L + 1442695040888963407L;\n"
		"	}\n"
		"	return state;\n"
		"}\n"
		"void main() { print(lcg(5)); }\n", TARGET_WINDOWS);
	/* state (r13) updated in place by imul then add against rbx-held constants. */
	ASSERT_INT(strstr(g_asm, "imul r13, rbx") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "add r13, rbx") != NULL, 1);
	/* The loop induction is a bare in-place add, not a rax round-trip. */
	ASSERT_INT(strstr(g_asm, "add r12, 1") != NULL, 1);
}

static void test_register_operand_compare(void)
{
	/* The loop test compares two promoted registers directly: cmp r12, r14,
	   with no `mov rax, r12` staging the LHS. */
	emit(
		"long lcg(long iters)\n"
		"{\n"
		"	long state; state = 1;\n"
		"	for (long i = 0; i < iters; i = i + 1) { state = state + 1; }\n"
		"	return state;\n"
		"}\n"
		"void main() { print(lcg(5)); }\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "cmp r12, r14") != NULL, 1);
}

static void test_nonneg_division_drops_sign_bias(void)
{
	/* Inside `while (n > 1)`, n is provably > 0, so n / 2 emits a bare shift with
	   no sign-bias (`sar rdx, 63`). An UNguarded signed /2 keeps the bias. */
	emit(
		"long f(long n)\n"
		"{\n"
		"	long acc; acc = 0;\n"
		"	while (n > 1) { acc = acc + (n / 2); n = n - 1; }\n"
		"	return acc;\n"
		"}\n"
		"void main() { print(f(10)); }\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "sar rdx, 63") == NULL, 1);   /* bias dropped under the guard. */

	emit(
		"long g(long n) { return n / 2; }\n"
		"void main() { print(g(10)); }\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "sar rdx, 63") != NULL, 1);   /* unguarded signed /2 keeps the bias. */
}

static void test_loop_rotation_bottom_test(void)
{
	/* A while loop is rotated to a bottom-tested form: the condition becomes an
	   entry guard (jump-if-false to end) plus a conditional back-edge (jump-if-true
	   to top), dropping the unconditional jmp. For `while (n > 1)` that means the
	   condition compare is emitted twice and a `jg` (jump-if-true) back-edge appears
	   where the old form had only a `jle` guard + `jmp`. */
	emit(
		"long f(long n)\n"
		"{\n"
		"	long s; s = 0;\n"
		"	while (n > 1) { s = s + n; n = n - 1; }\n"
		"	return s;\n"
		"}\n"
		"void main() { print(f(5)); }\n", TARGET_WINDOWS);
	int count = 0;
	const char *p = g_asm;
	while ((p = strstr(p, "cmp r12, 1")) != NULL)
	{
		count++;
		p++;
	}

	ASSERT_INT(count, 2);                              /* guard + bottom test. */
	ASSERT_INT(strstr(g_asm, "jg .L") != NULL, 1);    /* conditional back-edge. */
}

static void test_array_elem_stride(void)
{
	/* int[] indexing must use a *4 stride (32-bit elements), not *8. */
	emit(
		"void main()\n"
		"{\n"
		"	int[] a;\n"
		"	a = new int[2];\n"
		"	a[0] = 99;\n"
		"	int x;\n"
		"	x = a[0];\n"
		"}\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "*4 + 32]") != NULL, 1);   /* int[] uses 4-byte stride. */
	ASSERT_INT(strstr(g_asm, "*8 + 32]") == NULL, 1);   /* No 8-byte stride for int[]. */

	/* long[] indexing must use a *8 stride (64-bit elements). */
	emit(
		"void main()\n"
		"{\n"
		"	long[] b;\n"
		"	b = new long[2];\n"
		"	b[0] = 99L;\n"
		"	long y;\n"
		"	y = b[0];\n"
		"}\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "*8 + 32]") != NULL, 1);   /* long[] uses 8-byte stride. */
}

int main(void)
{
	RUN(test_array_elem_stride);
	RUN(test_loop_rotation_bottom_test);
	RUN(test_nonneg_division_drops_sign_bias);
	RUN(test_register_operand_compare);
	RUN(test_inplace_register_arithmetic);
	RUN(test_promotion_register_resident);
	RUN(test_linux_arg_regs);
	RUN(test_windows_arg_regs_unchanged);
	RUN(test_linux_receiver_and_release);
	RUN(test_linux_fp_arg_numbering);
	RUN(test_static_rsp_no_push_no_realign);
	RUN(test_devirt_monomorphic_direct);
	RUN(test_devirt_polymorphic_stays_indirect);
	RUN(test_devirt_unoverridden_base_method);
	RUN(test_bitwise_emission);
	RUN(test_logical_emission);
	RUN(test_div_strength_reduction);
	RUN(test_branch_fusion);
	RUN(test_divisibility_test);
	SUMMARY();
	return 0;
}
