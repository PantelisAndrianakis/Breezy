#include "test_framework.h"
#include "parser.h"
#include "types.h"
#include "resolve.h"
#include "codegen.h"
#include "prelude.h"

#define MAX_U 64
static char g_asm[1 << 16];

/* Compile `src` for `target` (prelude + the user unit, exactly as the driver
   does) and load the emitted asm into g_asm. */
static void emit(const char *src, Target target)
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

int main(void)
{
	RUN(test_linux_arg_regs);
	RUN(test_windows_arg_regs_unchanged);
	RUN(test_linux_receiver_and_release);
	RUN(test_linux_fp_arg_numbering);
	RUN(test_static_rsp_no_push_no_realign);
	SUMMARY();
	return 0;
}
