#include "test_framework.h"
#include "parser.h"
#include "types.h"
#include "resolve.h"
#include "codegen.h"
#include "config.h"   /* bzy_config_reset_cache: toggle BZY_IR for the SIMD case. */
#include "prelude.h"
#include "overload.h"
#include "grow.h"
#include <stdlib.h>   /* putenv: force the emitter for these emitter-asm assertions. */

#define MAX_U 64
/* Holds the full emitted program: the always-on prelude (vectors + DateTime's
   civil-date math) already exceeds 100 KB of asm, and the desktop tests add the
   GUI prelude on top, so this must stay well clear of that total or trailing
   output (e.g. vtables emitted after the prelude) gets silently truncated. */
static char g_asm[1 << 20];

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
	types_reserve_hashable(&tt);   /* Mirror main.c: slots 0/1 for record hashCode/equals, before interfaces. */
	for (int i = 0; i < total; i++)
	{
		types_register_interfaces(&tt, units[i]);
	}
	types_register_all_members(&tt, units, total);
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

/* Build a resolved TypeTable from source(s) without emitting, for analyses that
   consume the type table directly (e.g. the cycle analysis). Mirrors emit_n's
   table construction. */
static TypeTable *build_tt(const char **srcs, int nsrc)
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
	for (int i = 0; i < total; i++) { types_register_unit_names(&tt, units[i]); }
	types_reserve_hashable(&tt);
	for (int i = 0; i < total; i++) { types_register_interfaces(&tt, units[i]); }
	types_register_all_members(&tt, units, total);
	resolve_program(&tt, units, total);
	return &tt;
}

/* Scope a g_asm search to one emitted function's body. The user units' free
   functions are emitted before the always-on prelude classes, and each function
   is followed by its __exception metadata block, so the slice from `label` up to
   the next "__exception" is exactly that function's instructions. This excludes
   prelude code (e.g. the idiv / sar rdx, 63 that DateTime's civil-date math
   emits) that would otherwise pollute a whole-program substring search. The
   snippets scoped this way contain no try/catch, so their bodies never mention
   an __exception label themselves. Returns a pointer into a static buffer. */
static const char *fn_body(const char *label)
{
	static char buf[1 << 14];
	buf[0] = '\0';
	const char *s = strstr(g_asm, label);
	if (!s)
	{
		return buf;
	}
	const char *e = strstr(s, "__exception");
	size_t len = e ? (size_t)(e - s) : strlen(s);
	if (len >= sizeof(buf))
	{
		len = sizeof(buf) - 1;
	}
	memcpy(buf, s, len);
	buf[len] = '\0';
	return buf;
}

/* Compile prelude + desktop-prelude + the user source, exactly as the driver
   does when Desktop is referenced, and load the emitted asm into g_asm. */
static void emit_desktop(const char *src, Target target)
{
	static Parser parsers[MAX_U];
	static Unit *units[MAX_U];
	static TypeTable tt;

	int n = 0;
	for (int i = 0; i < BZY_PRELUDE_COUNT; i++)
	{
		parser_init(&parsers[n], BZY_PRELUDE[i]);
		units[n] = parse_unit(&parsers[n]);
		n++;
	}
	for (int i = 0; i < BZY_DESKTOP_PRELUDE_COUNT; i++)
	{
		parser_init(&parsers[n], BZY_DESKTOP_PRELUDE[i]);
		units[n] = parse_unit(&parsers[n]);
		n++;
	}
	parser_init(&parsers[n], src);
	units[n] = parse_unit(&parsers[n]);
	n++;

	types_init(&tt);
	types_register_builtins(&tt);
	for (int i = 0; i < n; i++) { types_register_unit_names(&tt, units[i]); }
	types_reserve_hashable(&tt);
	for (int i = 0; i < n; i++) { types_register_interfaces(&tt, units[i]); }
	types_register_all_members(&tt, units, n);
	resolve_program(&tt, units, n);

	FILE *f = fopen("out_cg_test.asm", "w+");
	if (!f) { printf("FAIL\n    cannot open out_cg_test.asm\n"); exit(1); }
	Codegen cg;
	cg_init(&cg, f);
	cg.target = target;
	cg_program(&cg, &tt, units, n);
	fflush(f);
	rewind(f);
	size_t got = fread(g_asm, 1, sizeof(g_asm) - 1, f);
	g_asm[got] = '\0';
	fclose(f);
}

static void test_desktop_is_enabled_lowers(void)
{
	emit_desktop("void main() { bool e; e = Desktop.isEnabled(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_desktop_is_enabled") != NULL, 1);
}

static void test_desktop_button_listener_lowers(void)
{
	emit_desktop(
		"class L implements ActionListener { void actionPerformed(ActionEvent e) { print(1); } }"
		" void main() {"
		"   Frame f; f = new Frame(); f.setTitle(\"Hi\");"
		"   Button b; b = new Button(); b.setText(\"Go\");"
		"   b.addActionListener(new L());"
		"   f.add(b); f.setSize(200, 100); f.show();"
		" }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_desktop_frame_new") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_desktop_button_new") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_desktop_listen_action") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_desktop_border_add") != NULL, 1);
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
	/* Bitwise by a fits-imm32 constant lowers to the immediate form (no rbx
	   staging): `and/or/xor rax, IMM`. */
	emit("void main() { int a; a = 6 & 3; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "and rax, 3") != NULL, 1);

	emit("void main() { int a; a = 6 | 1; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "or rax, 1") != NULL, 1);

	emit("void main() { int a; a = 5 ^ 1; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "xor rax, 1") != NULL, 1);

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

static void test_mul_strength_reduction(void)
{
	/* '*' by a power-of-two literal becomes a left shift by log2 of the constant
	   rather than `mov rbx, C; imul`. (A bare `imul` also appears in the runtime
	   prelude, so the test asserts the specific shift the reduction emits.) */
	emit("void main() { int n; n = 100; int a; a = n * 4; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "shl rax, 2") != NULL, 1);

	emit("void main() { int n; n = 100; int a; a = n * 8; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "shl rax, 3") != NULL, 1);
}

static void test_fp_leaf_fusion(void)
{
	/* A same-type FP leaf rhs folds into the op as a memory operand instead of
	   being spilled/reloaded through a scratch slot: `a * b` -> mulsd with a qword
	   memory source. */
	emit("void main() { double a; double b; double c; a = 1.5; b = 2.5; c = a * b; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "mulsd xmm0, qword") != NULL, 1);

	/* `lhs + A*B` for locals becomes a multiply-accumulate into xmm1, combined
	   into xmm0 with no lhs spill (a separate mul+add, not an FMA). */
	emit("void main() { double a; double b; double c; double r; a = 1.5; b = 2.5; c = 3.5;"
		 " r = c + a * b; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "mulsd xmm1, qword") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "addsd xmm0, xmm1") != NULL, 1);
}

static void test_inplace_mac(void)
{
	/* A multiply-accumulate `acc = acc + a[i]*a[i]` onto a promoted accumulator
	   evaluates the product into rax and adds it straight into the accumulator's
	   callee-saved register - no stack spill of the accumulator across the rhs. */
	emit("long f(long[] a) { long acc; acc = 0; long i; i = 0;"
		 " while (i < 4) { acc = acc + a[i] * a[i]; i = i + 1; } return acc; }"
		 " void main() { }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "imul rax, rbx\n    add r1") != NULL, 1);
}

static void test_register_index_addr(void)
{
	/* A register-resident index (a promoted loop variable) is bounds-checked from
	   its register (cmp reg,[len]) and the element address folds straight into the
	   load - no separate lea into rbx, no base spill or rematerialization into rax.
	   The array base pointer is also loop-invariant, so it is hoisted into a
	   caller-saved register (r8) and used directly without a mov rax,r8 round-trip. */
	emit("int f(int[] a, int n) { int s; s = 0; int i; i = 0;"
		 " while (i < n) { s = s + a[i]; i = i + 1; } return s; } void main() { }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "cmp r12, [r8 + 24]") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "[r8 + r12*4 + 32]") != NULL, 1);       /* Address folded into the access. */
	ASSERT_INT(strstr(g_asm, "lea rbx, [r8 + r12*4 + 32]") != NULL, 0); /* No separate lea-into-rbx. */
}

/* Count `needle` occurrences inside the emitted body of one function, bounded by
   the function's label and the next top-level `bzy_` function label. */
static int count_in_func(const char *func_label, const char *needle)
{
	const char *s = strstr(g_asm, func_label);
	if (!s)
	{
		return -1;
	}

	s += strlen(func_label);
	const char *end = strstr(s, "\nbzy_");
	int c = 0;
	const char *p = s;
	while ((p = strstr(p, needle)) != NULL && (!end || p < end))
	{
		c++;
		p += strlen(needle);
	}

	return c;
}

static void test_boundscheck_cse_repeated_index(void)
{
	/* a[i] read twice, same array and same index, with no store between: the first
	   access bounds-checks; the second is provably in range because the first guards
	   it (an out-of-range index throws at the first, so the second only runs when in
	   range). Intra-block CSE drops the redundant check, so exactly ONE bzy_oob slow
	   path is emitted for the pair, not two. i is a parameter (no interval fact), so
	   without CSE both accesses would each emit a check. */
	emit("long f(long[] a, int i) { return a[i] + a[i]; } void main() { }", TARGET_LINUX);
	ASSERT_INT(count_in_func("bzy_f:", "call bzy_oob"), 1);
}

static void test_foreach_base_hoist(void)
{
	/* foreach over a value List with a call-free, nested-loop-free body: the
	   collection's data base, length, head and cap are loop-invariant, so they are
	   hoisted into r8..r11 once before the loop instead of being reloaded through
	   the collection pointer (rdx) every iteration. The element load addresses the
	   hoisted data base directly, and the length test compares against the hoisted
	   length register - no per-iteration [rdx + 24] / [rdx + 48] reload. */
	emit(
		"void main() {"
		"  List<long> xs; xs = new List<long>();"
		"  int i; for (i = 0; i < 8; i = i + 1) { xs.add((long)i); }"
		"  long s; s = 0;"
		"  foreach (long x in xs) { s = s + x; }"
		"}",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "mov r9, [rdx + 48]") != NULL, 1);    /* data base hoisted */
	ASSERT_INT(strstr(g_asm, "mov r10, [rdx + 24]") != NULL, 1);   /* length hoisted */
	ASSERT_INT(strstr(g_asm, "cmp rcx, r10") != NULL, 1);          /* length test from register */
	ASSERT_INT(strstr(g_asm, "[r9 + rcx*8 + 32]") != NULL, 1);     /* element via hoisted base */
}

static void test_map_foreach_snapshot_handle(void)
{
	/* Map foreach iterates through an owned snapshot handle (identity when the
	   map is confined, a frozen clone when shared, so a concurrent rehash can
	   never invalidate the slot cursor); the handle is released at loop exit. */
	emit(
		"void main() {"
		"  map<int,int> m; m = new map<int,int>();"
		"  m.put(1, 10);"
		"  int s; s = 0;"
		"  foreach (int k in m) { s = s + k; }"
		"}",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_map_iter_snapshot") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "call bzy_map_iter") != NULL, 1);
}

static void test_shared_set_closure(void)
{
	/* A class reachable only through a builtin-container channel element, a
	   spawned function's parameter, or a static field must still allocate with
	   the SHARED gcinfo literal (atomic refcounts). */
	/* Every snippet allocates through an escaping `make()` (a non-escaping local
	   is stack-allocated and never carries the literal), and the channel appears
	   in a signature (a purely local channel can never reach another breeze, so
	   signatures + statics + spawns are the complete seed set). */
	emit(
		"class Dog { int age; }"
		"Dog make() { return new Dog(); }"
		"void pump(channel<List<Dog>> ch) { }"
		"void main() { Dog d; d = make(); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "or qword [rax + 16], 8") != NULL, 1);

	emit(
		"class Dog { int age; }"
		"Dog make() { return new Dog(); }"
		"void feed(Dog d) { }"
		"void main() { Dog d; d = make(); spawn feed(d); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "or qword [rax + 16], 8") != NULL, 1);

	emit(
		"class Dog { int age; }"
		"Dog make() { return new Dog(); }"
		"class Kennel { static Dog mascot; }"
		"void main() { Kennel.mascot = make(); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "or qword [rax + 16], 8") != NULL, 1);

	/* Negative control: an escaping allocation of a class that never crosses
	   cores stays non-shared (no literal). */
	emit(
		"class Dog { int age; }"
		"Dog make() { return new Dog(); }"
		"void main() { Dog d; d = make(); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "or qword [rax + 16], 8") == NULL, 1);
}

static void test_shared_array_gating(void)
{
	/* Managed-element array sites whose static type may cross cores emit a
	   SHARED-bit test routing to the locked slot helpers; value arrays and
	   types that can never be shared emit no gating at all. */
	emit(
		"void pump(channel<string[]> ch) { }"
		"void main() { string[] a; a = new string[4]; a[1] = \"x\"; string s; s = a[1]; }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_array_get_shared") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "call bzy_array_set_shared") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "test qword [rax + 16], 8") != NULL, 1);

	/* Same code, no share point: zero gating bytes (the extern declarations are
	   unconditional, so match the calls). */
	emit(
		"void main() { string[] a; a = new string[4]; a[1] = \"x\"; string s; s = a[1]; }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_array_get_shared") == NULL, 1);
	ASSERT_INT(strstr(g_asm, "call bzy_array_set_shared") == NULL, 1);

	/* Value arrays never gate, even when the type may be shared. */
	emit(
		"void pump(channel<int[]> ch) { }"
		"void main() { int[] a; a = new int[4]; a[1] = 7; int x; x = a[1]; }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_array_get_shared") == NULL, 1);
	ASSERT_INT(strstr(g_asm, "call bzy_array_set_shared") == NULL, 1);
}

static void test_vec_gating_and_store_barriers(void)
{
	/* Inlined vec fast paths (get/set/add of value elements) gate on the
	   receiver's SHARED bit when the List type may cross cores; the gated route
	   is the shared-aware runtime call. */
	emit(
		"void pump(channel<List<int>> ch) { }"
		"void main() { List<int> l; l = new List<int>(); l.add(7); l.set(0, 8); int x; x = l.get(0); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "test qword [rdx + 16], 8") != NULL, 1);

	/* No share point: the inline fast paths stay test-free. */
	emit(
		"void main() { List<int> l; l = new List<int>(); l.add(7); l.set(0, 8); int x; x = l.get(0); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "test qword [rdx + 16], 8") == NULL, 1);

	/* A managed store to a static slot deep-shares the value before publishing. */
	emit(
		"class Holder { static List<int> cache; }"
		"void main() { List<int> mine; mine = new List<int>(); Holder.cache = mine; }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_share_crosscore") != NULL, 1);

	/* A managed store into a field of a shared class deep-shares the value. */
	emit(
		"class Box { List<int> items; }"
		"void pump(channel<Box> ch) { }"
		"void main() { Box b; b = new Box(); b.items = new List<int>(); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_share_crosscore") != NULL, 1);

	/* No share reachability: a plain field store emits no share call. */
	emit(
		"class Box { List<int> items; }"
		"void main() { Box b; b = new Box(); b.items = new List<int>(); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_share_crosscore") == NULL, 1);
}

static void test_subclass_declared_before_parent(void)
{
	/* Class member registration must be parent-first regardless of declaration
	   (or file) order: a subclass registered against a name-only parent copies
	   zeroed layout, truncating its vtable and misnumbering its slots - the
	   Linux proj_inherit segfault (ext4 readdir order put Dog.bzy first). */
	emit(
		"class Dog extends Animal { void speak() { print(2); } }"
		"class Animal { int sound; void speak() { print(1); } }"
		"void main() { Animal a; a = new Dog(); a.speak(); }",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm,
		"__vtable_Dog:\n"
		"    dq 0\n"
		"    dq 0\n"
		"    dq Dog__speak") != NULL, 1);   /* Full table: reserved slots 0/1, speak at slot 2. */
}

static void test_map_compound_lowering(void)
{
	/* putIfAbsent/getOrDefault lower to the atomic runtime intrinsics. */
	emit(
		"void main() {"
		"  map<int,int> m; m = new map<int,int>();"
		"  int x; x = m.putIfAbsent(1, 10);"
		"  int y; y = m.getOrDefault(1, 5);"
		"  print(x + y);"
		"}",
		TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_map_put_if_absent") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "call bzy_map_get_or_default") != NULL, 1);
}

static void test_div_strength_reduction(void)
{
	/* '/' by a power-of-two literal becomes an (arithmetic) shift, not idiv. */
	emit("void main() { int n; n = 100; int a; a = n / 2; }", TARGET_LINUX);
	ASSERT_INT(strstr(fn_body("bzy_user_main:"), "idiv") == NULL, 1);
	ASSERT_INT(strstr(g_asm, "sar rax, 1") != NULL, 1);

	/* '%' by a power-of-two literal becomes a mask, not idiv. */
	emit("void main() { int n; n = 100; int a; a = n % 2; }", TARGET_LINUX);
	ASSERT_INT(strstr(fn_body("bzy_user_main:"), "idiv") == NULL, 1);
	ASSERT_INT(strstr(g_asm, "and rax, 1") != NULL, 1);

	/* Unsigned divide uses a logical shift (no sign bias). */
	emit("void main() { uint n; n = 100u; uint a; a = n / 4; }", TARGET_LINUX);
	ASSERT_INT(strstr(fn_body("bzy_user_main:"), "idiv") == NULL, 1);
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
	   asserts the exact fused pair instead. `n` is an int local, now promoted to a
	   register (see promote.c), so the fused compare reads it straight from r12. An
	   int comparison uses the 32-bit subregister (r12d): the low 32 bits carry the
	   int value whether or not the upper half is a valid sign-extension. */
	emit("void main() { int n; n = 5; if (n > 1) { print(1); } }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "cmp r12d, 1\n    jle") != NULL, 1);

	emit("void main() { int n; n = 5; if (n < 3) { print(1); } }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "cmp r12d, 3\n    jge") != NULL, 1);
}

static void test_unroll_folds_derived_constant(void)
{
	/* cb = u * 8 inside an unrolled 8-trip loop: u is a per-copy constant, so
	   cb must fold to a literal - no shl (the *8 strength reduction) may
	   survive anywhere in the function. */
	emit("void main()\n"
		 "{\n"
		 "	int[] a;\n"
		 "	a = new int[64];\n"
		 "	long t;\n"
		 "	t = 0;\n"
		 "	for (int u = 0; u < 8; u = u + 1)\n"
		 "	{\n"
		 "		int cb;\n"
		 "		cb = u * 8;\n"
		 "		t = t + (long)a[cb];\n"
		 "	}\n"
		 "	print(\"\" + t);\n"
		 "}\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "shl") == NULL, 1);
}

static void test_nested_unroll_outer_constant_survives_inner(void)
{
	/* After an inner unrolled loop ends, statements still inside the OUTER
	   unrolled copy must still see the outer induction constant (the old code
	   zeroed the unroll state on inner exit). r * 100 after the inner loop
	   must fold: the r = 3 copy materializes 300, and no imul by 100 remains. */
	emit("void main()\n"
		 "{\n"
		 "	long t;\n"
		 "	t = 0;\n"
		 "	for (int r = 0; r < 4; r = r + 1)\n"
		 "	{\n"
		 "		for (int u = 0; u < 4; u = u + 1)\n"
		 "		{\n"
		 "			t = t + (long)u;\n"
		 "		}\n"
		 "		t = t + (long)(r * 100);\n"
		 "	}\n"
		 "	print(\"\" + t);\n"
		 "}\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, ", 300") != NULL, 1);
	/* Prelude functions legitimately multiply; only main's own body (label to
	   its epilogue ret) must be imul-free. */
	const char *m = strstr(g_asm, "main:");
	const char *mend = m ? strstr(m, "\n    ret") : NULL;
	ASSERT_INT(m != NULL && mend != NULL, 1);
	int main_imuls = 0;
	for (const char *p = strstr(m, "imul"); p && p < mend; p = strstr(p + 1, "imul"))
	{
		main_imuls++;
	}

	ASSERT_INT(main_imuls, 0);
}

static void test_unroll_constant_index_direct_disp(void)
{
	/* a[cb + xp] with cb and xp both copy-constants folds to one direct
	   address: element 63 = byte offset 32 + 63*4 = 284 appears as a literal
	   displacement, with no index-register arithmetic for it. */
	emit("void main()\n"
		 "{\n"
		 "	int[] a;\n"
		 "	a = new int[64];\n"
		 "	long t;\n"
		 "	t = 0;\n"
		 "	for (int u = 0; u < 8; u = u + 1)\n"
		 "	{\n"
		 "		int cb;\n"
		 "		cb = u * 8;\n"
		 "		for (int xp = 0; xp < 8; xp = xp + 1)\n"
		 "		{\n"
		 "			t = t + (long)a[cb + xp];\n"
		 "		}\n"
		 "	}\n"
		 "	print(\"\" + t);\n"
		 "}\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "+ 284]") != NULL, 1);
}

static void test_unrolled_mac_memory_operand_imul(void)
{
	/* The codec MAC shape: both multiplicands are safe array loads with
	   foldable addresses, so the multiply takes its second operand straight
	   from memory - no park of the first operand in a scratch slot. */
	emit("void main()\n"
		 "{\n"
		 "	int[] a;\n"
		 "	a = new int[64];\n"
		 "	int[] b;\n"
		 "	b = new int[64];\n"
		 "	long t;\n"
		 "	t = 0;\n"
		 "	for (int u = 0; u < 8; u = u + 1)\n"
		 "	{\n"
		 "		int acc;\n"
		 "		acc = 0;\n"
		 "		for (int xp = 0; xp < 8; xp = xp + 1)\n"
		 "		{\n"
		 "			acc = acc + a[u * 8 + xp] * b[u * 8 + xp];\n"
		 "		}\n"
		 "		t = t + (long)acc;\n"
		 "	}\n"
		 "	print(\"\" + t);\n"
		 "}\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "imul eax, dword [") != NULL, 1);
}

static void test_unroll_kill_not_resurrected_by_inner_exit(void)
{
	/* acc is recorded (acc = 0), killed by the accumulation inside the inner
	   unrolled loop, and read after that loop exits. The inner loop's uc_n
	   snapshot restore must not resurrect the killed entry: every copy's
	   acc >> 1 computes for real - 4 sar instructions, not 1. */
	emit("void main()\n"
		 "{\n"
		 "	int[] a;\n"
		 "	a = new int[8];\n"
		 "	long total;\n"
		 "	total = 0;\n"
		 "	for (int u = 0; u < 4; u = u + 1)\n"
		 "	{\n"
		 "		int acc;\n"
		 "		acc = 0;\n"
		 "		for (int xp = 0; xp < 8; xp = xp + 1)\n"
		 "		{\n"
		 "			acc = acc + a[xp];\n"
		 "		}\n"
		 "		total = total + (long)(acc >> 1);\n"
		 "	}\n"
		 "	print(\"\" + total);\n"
		 "}\n", TARGET_WINDOWS);
	int sars = 0;
	const char *body = fn_body("bzy_user_main:");
	for (const char *p = strstr(body, "sar "); p; p = strstr(p + 1, "sar "))
	{
		sars++;
	}

	ASSERT_INT(sars, 4);
}

static void test_divisibility_test(void)
{
	/* (X % 2^k) == 0 / != 0 in a condition collapses to a single mask that sets ZF -
	   no idiv, no signed-remainder reconstruction. == 0 inverts to jne, != 0 to je. */
	emit("void main() { int n; n = 5; if (n % 2 == 0) { print(1); } }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "and rax, 1\n    jne") != NULL, 1);
	ASSERT_INT(strstr(fn_body("bzy_user_main:"), "idiv") == NULL, 1);

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

static void test_float_promotion_xmm(void)
{
	/* A call-free loop accumulating double temps promotes them to xmm2..xmm5
	   (caller-saved on both ABIs, so no prologue save is needed). The multiply then
	   reads its operand straight from a register instead of the local's [rbp] slot. */
	emit(
		"double dot(double[] a, int n)\n"
		"{\n"
		"	double acc; acc = 0.0;\n"
		"	for (int i = 0; i < n; i = i + 1)\n"
		"	{\n"
		"		double x; x = a[i];\n"
		"		acc = acc + x * x;\n"
		"	}\n"
		"	return acc;\n"
		"}\n"
		"void main() { double[] a; a = new double[4]; print((long)dot(a, 4)); }\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "xmm2") != NULL, 1);            /* a hot double promoted to xmm2. */
	ASSERT_INT(strstr(g_asm, "mulsd xmm1, xmm") != NULL, 1); /* MAC operand read from a register, not a slot. */
}

static void test_fp_indexed_load_direct_addressing(void)
{
	/* A BCE-safe double[] access in a hoisted-base counted loop loads with one
	   instruction - movsd from the full [base + index*8 + disp] addressing mode -
	   instead of materializing the address in rbx with a lea first. Mirrors the
	   matrix kernel's flattened vertex sweep. */
	emit(
		"void main()\n"
		"{\n"
		"	int n; n = 100;\n"
		"	double[] a; a = new double[n * 2];\n"
		"	double m0; m0 = 1.5;\n"
		"	double m1; m1 = 2.5;\n"
		"	double acc; acc = 0.0;\n"
		"	for (int i = 0; i < n; i = i + 1)\n"
		"	{\n"
		"		int b; b = i * 2;\n"
		"		double x; x = a[b + 0];\n"
		"		double y; y = a[b + 1];\n"
		"		acc = acc + (m0 * x + m1 * y);\n"
		"	}\n"
		"	print((long)acc);\n"
		"}\n", TARGET_WINDOWS);
	/* The element load folds base, scaled index, and header displacement into the
	   movsd itself (r8 holds the hoisted base, the index lives in a register). */
	ASSERT_INT(strstr(g_asm, ", qword [r8 + r") != NULL, 1);
}

static void test_loop_scoped_float_promotion(void)
{
	/* A double accumulator whose live range crosses a call OUTSIDE the inner loop
	   cannot take a caller-saved xmm2..5 home, but the call-free inner loop
	   promotes it into callee-saved xmm6..xmm11 for the loop's span: load before
	   the loop, register arithmetic inside, store back after. On Windows the
	   clobbered callee-saved registers are preserved around the loop with movups;
	   on SysV they are volatile and need no preservation. */
	const char *src =
		"void main()\n"
		"{\n"
		"	int n; n = 100;\n"
		"	double[] a; a = new double[n * 2];\n"
		"	double acc; acc = 0.0;\n"
		"	for (int f = 0; f < 3; f = f + 1)\n"
		"	{\n"
		"		long t; t = Clock.currentTimeNanos();\n"
		"		for (int i = 0; i < n; i = i + 1)\n"
		"		{\n"
		"			int b; b = i * 2;\n"
		"			double x; x = a[b + 0];\n"
		"			double y; y = a[b + 1];\n"
		"			acc = acc + (x * y);\n"
		"		}\n"
		"		print(t);\n"
		"	}\n"
		"	print((long)acc);\n"
		"}\n";
	emit(src, TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "movsd xmm6, qword [rbp - ") != NULL, 1);   /* Preheader load. */
	ASSERT_INT(strstr(g_asm, "], xmm6") != NULL, 1);                      /* Exit store to the slot. */
	ASSERT_INT(strstr(g_asm, "movups") != NULL, 1);                       /* Win64: xmm6 is callee-saved. */
	emit(src, TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "movsd xmm6, qword [rbp - ") != NULL, 1);   /* Same promotion on SysV... */
	ASSERT_INT(strstr(g_asm, "movups") == NULL, 1);                       /* ...without the save/restore. */
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
	ASSERT_INT(strstr(fn_body("bzy_f:"), "sar rdx, 63") == NULL, 1);   /* bias dropped under the guard. */

	emit(
		"long g(long n) { return n / 2; }\n"
		"void main() { print(g(10)); }\n", TARGET_WINDOWS);
	ASSERT_INT(strstr(fn_body("bzy_g:"), "sar rdx, 63") != NULL, 1);   /* unguarded signed /2 keeps the bias. */
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
	const char *p = fn_body("bzy_f:");
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

static void test_inline_arc_fast_paths(void)
{
	/* retain and release inline their common cases instead of always calling the
	   runtime. A borrowed field store emits both: retain of the new occupant,
	   release of the old. The inline retain ends in a recolor-to-BLACK mask; the
	   inline release probes the type descriptor's child count to decide whether a
	   survivor must be buffered. The runtime calls remain as the slow path (shared
	   objects, refcount reaching zero, survivors with children). */
	emit("class A { int v; }"
		 " void main() { A x; x = new A(); A[] arr; arr = new A[2]; arr[0] = x; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "and qword [rax + 16], -4") != NULL, 1);  /* inline retain: set_color BLACK. */
	ASSERT_INT(strstr(g_asm, "cmp qword [rdx + 8], 0") != NULL, 1);    /* inline release: leaf/child probe. */
	ASSERT_INT(strstr(g_asm, "cmp qword [rdx], 0") != NULL, 1);        /* inline free: finalizer probe. */
	ASSERT_INT(strstr(g_asm, "rax*4 + 88], 256") != NULL, 1);         /* inline free: n[c] < POOL_CAP push. */
	ASSERT_INT(strstr(g_asm, "test dl, 8") != NULL, 1);                /* inline retain: SHARED-bit check. */
	ASSERT_INT(strstr(g_asm, "test al, 8") != NULL, 1);                /* inline release: SHARED-bit check. */
	ASSERT_INT(strstr(g_asm, "call bzy_retain") != NULL, 1);           /* slow path retained. */
	ASSERT_INT(strstr(g_asm, "call bzy_release") != NULL, 1);          /* slow path retained. */
}

static void test_inline_alloc_windows_pool_pop(void)
{
	/* On Windows a `new` of a poolable class inlines bzy_alloc's hot path: gate on
	   the TEB-slot-ready flag, read the per-thread pool pointer from the TEB slot,
	   and pop the size-class free list - with a bzy_alloc fallback kept for the cold
	   cases. Linux keeps the plain call (no gs:-relative pool pop). */
	/* The object must escape (stored into the array) so it heap-allocates rather
	   than landing on the stack via escape analysis. */
	emit("class A { int v; } void main() { A[] arr; arr = new A[1]; arr[0] = new A(); }", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "[rel bzy_pool_slot_ready]") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "[gs:0x1480 + rax*8]") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "call bzy_alloc") != NULL, 1);   /* Cold-path fallback retained. */

	emit("class A { int v; } void main() { A[] arr; arr = new A[1]; arr[0] = new A(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "gs:0x1480") == NULL, 1);        /* Linux uses fs:, not the TEB slot. */
	ASSERT_INT(strstr(g_asm, "[rel bzy_tpool_off]") != NULL, 1);   /* Linux inline: TLS-offset base. */
	ASSERT_INT(strstr(g_asm, "[fs:rcx + 8]") != NULL, 1);          /* Linux inline: pop head[1] of t_pool. */
	ASSERT_INT(strstr(g_asm, "call bzy_alloc") != NULL, 1);
}

/* ---- Overload module (pure: signature encoding + best-match selection). ---- */

static void test_overload_encode(void)
{
	TypeRef ii[2] = { { .kind = TY_INT }, { .kind = TY_INT } };
	char buf[64];
	overload_encode_types(buf, sizeof(buf), ii, 2);
	ASSERT_STR(buf, "ii");

	TypeRef none[1] = { { .kind = TY_VOID } };
	overload_encode_types(buf, sizeof(buf), none, 0);
	ASSERT_STR(buf, "v");

	TypeRef obj[1] = { { .kind = TY_OBJECT } };
	strcpy(obj[0].class_name, "Dog");
	overload_encode_types(buf, sizeof(buf), obj, 1);
	ASSERT_STR(buf, "O3Dog");
}

static void test_overload_select_exact_beats_widening(void)
{
	TypeRef pi[1] = { { .kind = TY_INT } };
	TypeRef pl[1] = { { .kind = TY_LONG } };
	OverloadCand cands[2] = { { pi, 1, 1, 0 }, { pl, 1, 1, 0 } };

	TypeRef arg_int[1] = { { .kind = TY_INT } };
	ASSERT_INT(overload_select(cands, 2, arg_int, 1), 0);   /* int arg -> f(int). */

	TypeRef arg_long[1] = { { .kind = TY_LONG } };
	ASSERT_INT(overload_select(cands, 2, arg_long, 1), 1);  /* long arg -> f(long). */
}

static void test_overload_select_null_ambiguous(void)
{
	TypeRef pa[1] = { { .kind = TY_OBJECT } };
	TypeRef pb[1] = { { .kind = TY_OBJECT } };
	strcpy(pa[0].class_name, "Dog");
	strcpy(pb[0].class_name, "Cat");
	OverloadCand cands[2] = { { pa, 1, 1, 0 }, { pb, 1, 1, 0 } };

	TypeRef arg_null[1] = { { .kind = TY_NULL } };
	ASSERT_INT(overload_select(cands, 2, arg_null, 1), OVL_AMBIG);

	TypeRef arg_dog[1] = { { .kind = TY_OBJECT } };
	strcpy(arg_dog[0].class_name, "Dog");
	ASSERT_INT(overload_select(cands, 2, arg_dog, 1), 0);   /* Exact class wins. */
}

static void test_overload_set_ambiguous(void)
{
	/* Box(int) [1,1] vs Box(int, int=0) [1,2] collide at arity 1. */
	TypeRef p1[1] = { { .kind = TY_INT } };
	TypeRef p2[2] = { { .kind = TY_INT }, { .kind = TY_INT } };
	OverloadCand amb[2] = { { p1, 1, 1, 0 }, { p2, 2, 1, 0 } };
	ASSERT_INT(overload_set_is_ambiguous(amb, 2), 1);

	/* f(int) vs f(string): never collide. */
	TypeRef ps[1] = { { .kind = TY_STRING } };
	OverloadCand ok[2] = { { p1, 1, 1, 0 }, { ps, 1, 1, 0 } };
	ASSERT_INT(overload_set_is_ambiguous(ok, 2), 0);
}

static void test_stack_args_caller(void)
{
	emit("int f(int a,int b,int c,int d,int e){return a+b+c+d+e;} void main(){ print(f(1,2,3,4,5)); }", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "[rsp + 32]") != NULL, 1);   /* 5th int arg spilled above the Win64 shadow. */
}

static void test_stack_args_callee(void)
{
	emit("int f(int a,int b,int c,int d,int e){return e;} void main(){ print(f(1,2,3,4,5)); }", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "[rbp + 48]") != NULL, 1);   /* 5th arg read from the caller frame. */
}

static void test_grow_ensure(void)
{
	int *a = NULL, cap = 0, n = 0;
	for (int i = 0; i < 100; i++)
	{
		a = grow_ensure(a, n, &cap, sizeof(int));
		a[n++] = i * 7;
	}
	ASSERT_INT(n, 100);
	ASSERT_INT(cap >= 100, 1);
	ASSERT_INT(a[0], 0);
	ASSERT_INT(a[99], 693);
}

static void test_ffi_fromcstring_lowers(void)
{
	/* fromCString(ptr) lowers to a call into the runtime bridge helper. */
	emit("extern long getptr(); void main() { string s; s = fromCString(getptr()); print(s); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_str_from_cstring") != NULL, 1);
}

static void test_ffi_frombytes_lowers(void)
{
	/* fromCBytes(ptr, len) lowers to a call into the length-counted bridge helper. */
	emit("extern long getptr(); void main() { string s; s = fromCBytes(getptr(), 3); print(s); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_str_from_cbytes") != NULL, 1);
}

static void test_ffi_array_arg_marshals_data_ptr(void)
{
	/* A value array passed to an extern marshals its element-data pointer (+32),
	   exactly like a string. arg0 is rdi on SysV. */
	emit("extern long sink(byte[] buf, long n);"
		 " void main() { byte[] b; b = new byte[4]; sink(b, 4); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "add rdi, 32") != NULL, 1);
}

static void test_ffi_blocking_five_args(void)
{
	/* A >4-arg blocking extern compiles (no abort) and emits its worker thunk. */
	emit("extern blocking long f5(long a, long b, long c, long d, long e);"
		 " void main() { long r; r = f5(1,2,3,4,5); }", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "__blocking_") != NULL, 1);
}

static void test_ffi_str_tobytes_lowers(void)
{
	/* SP2: string.toBytes() lowers to a bzy_str_to_bytes call (owned byte[]). */
	emit("void main() { string s; s = \"hi\"; byte[] b; b = s.toBytes(); print(b.length); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_str_to_bytes") != NULL, 1);
}

static void test_ffi_frombytes_arr_lowers(void)
{
	/* SP2: fromBytes(byte[]) lowers to a bzy_str_from_bytes call (owned string). */
	emit("void main() { byte[] b; b = new byte[3]; string s; s = fromBytes(b); print(s); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_str_from_bytes") != NULL, 1);
}

static void test_ffi_callback_type_lowers(void)
{
	/* SP3: a TY_FUNC (long(long,long)) extern param accepts a matching Breezy
	   function by name and lowers to its address (lea [rel bzy_cmp]). */
	emit("extern long qsort(long[] base, long n, long sz, long(long,long) cmp);"
		 " long cmp(long a, long b) { return a - b; }"
		 " void main() { long[] xs; xs = new long[2]; qsort(xs, 2, 8, cmp); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "lea rax, [rel bzy_cmp]") != NULL, 1);
}

static void test_ffi_variadic_parses(void)
{
	/* SP3: a variadic extern (trailing ...) parses and a call with extra args
	   beyond the fixed params compiles, setting AL=0 for integer-only varargs
	   (SysV variadic ABI: AL = vector-register count). */
	emit("extern int snprintf(byte[] buf, long size, string fmt, ...);"
		 " void main() { byte[] b; b = new byte[8]; snprintf(b, 8, \"%d\", 7); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "$snprintf") != NULL && strstr(g_asm, "mov al, 0") != NULL, 1);
}

static void test_ffi_getenv_lowers(void)
{
	/* SP4: System.getenv(name) lowers to a bzy_sys_getenv call (owned string). */
	emit("void main() { string p; p = System.getenv(\"PATH\"); print(p); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_sys_getenv") != NULL, 1);
}

static void test_ffi_await_shutdown_lowers(void)
{
	/* SP4: System.awaitShutdown() lowers to a bzy_await_shutdown call. */
	emit("void main() { System.awaitShutdown(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_await_shutdown") != NULL, 1);
}

static void test_system_realtime_io_lowers(void)
{
	/* System.sleep(ms) lowers to a bzy_sys_sleep call. */
	emit("void main() { System.sleep(16); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_sys_sleep") != NULL, 1);

	/* System.rawMode(on) lowers to a bzy_sys_raw_mode call. */
	emit("void main() { System.rawMode(true); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_sys_raw_mode") != NULL, 1);

	/* System.pollKey() lowers to a bzy_sys_poll_key call. */
	emit("void main() { int k; k = System.pollKey(); print(k); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_sys_poll_key") != NULL, 1);

	/* System.mouseMode(on) lowers to a bzy_sys_mouse_mode call. */
	emit("void main() { System.mouseMode(true); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_sys_mouse_mode") != NULL, 1);

	/* System.pollMouse() lowers to a bzy_sys_poll_mouse call. */
	emit("void main() { long m; m = System.pollMouse(); print(m); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_sys_poll_mouse") != NULL, 1);

	/* System.cpuCount() lowers to a bzy_sys_cpu_count call. */
	emit("void main() { int n; n = System.cpuCount(); print(n); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_sys_cpu_count") != NULL, 1);

	/* System.affinity(mask) lowers to a bzy_sys_affinity call. */
	emit("void main() { bool ok; ok = System.affinity(1); print(ok); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_sys_affinity") != NULL, 1);
}

static void test_filechannel_lock_lowers(void)
{
	/* FileChannel.lock() lowers to a bzy_filechannel_lock call. */
	emit("void main() { FileChannel c; c = File.openChannel(\"x.dat\"); bool ok; ok = c.lock(); print(ok); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_filechannel_lock") != NULL, 1);

	/* FileChannel.unlock() lowers to a bzy_filechannel_unlock call. */
	emit("void main() { FileChannel c; c = File.openChannel(\"x.dat\"); c.unlock(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_filechannel_unlock") != NULL, 1);
}

static void test_raw_socket_lowers(void)
{
	/* Network.rawSocket(protocol) lowers to a bzy_raw_socket call guarded by io_check. */
	emit("void main() { Socket s; s = Network.rawSocket(255); s.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_raw_socket") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_io_check") != NULL, 1);
}

static void test_tls_lowers(void)
{
	/* Network.tlsConnect (2-arg) lowers to bzy_tls_connect guarded by io_check. */
	emit("void main() { TlsSocket s; s = Network.tlsConnect(\"h\", 443); s.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_tls_connect") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_io_check") != NULL, 1);

	/* Network.tlsListen + accept + read/write/close lower to their bzy_tls_* calls. */
	emit("void main() { TlsListener l; l = Network.tlsListen(0, \"c.pem\", \"k.pem\"); TlsSocket s; s = l.accept(); byte[] b; b = s.read(16); s.write(b); s.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_tls_listen") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_tls_accept") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_tls_read") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_tls_write") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_tls_close") != NULL, 1);
}

static void test_dtls_lowers(void)
{
	/* Network.dtlsConnect (2-arg) lowers to bzy_dtls_connect guarded by io_check. */
	emit("void main() { DtlsSocket s; s = Network.dtlsConnect(\"h\", 4433); s.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_dtls_connect") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_io_check") != NULL, 1);

	/* dtlsConnectInsecure lowers to the loudly-named insecure entry point. */
	emit("void main() { DtlsSocket s; s = Network.dtlsConnectInsecure(\"h\", 4433); s.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_dtls_connect_insecure") != NULL, 1);

	/* Network.dtlsListen + accept + read/write/close lower to their bzy_dtls_* calls. */
	emit("void main() { DtlsListener l; l = Network.dtlsListen(0, \"c.pem\", \"k.pem\"); DtlsSocket s; s = l.accept(); byte[] b; b = s.read(16); s.write(b); s.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_dtls_listen") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_dtls_accept") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_dtls_read") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_dtls_write") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_dtls_close") != NULL, 1);
}

static void test_surface_lowers(void)
{
	/* Graphics.open lowers to bzy_surface_open guarded by io_check; the Surface
	   methods lower to their bzy_surface_* calls. */
	emit("void main() { Surface s; s = Graphics.open(64, 64, \"t\"); int[] fb; fb = new int[64*64]; s.present(fb); long e; e = s.pollEvent(); bool o; o = s.isOpen(); s.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_surface_open") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_io_check") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_surface_present") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_surface_poll_event") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_surface_is_open") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_surface_close") != NULL, 1);
}

static void test_glsurface_lowers(void)
{
	/* Graphics.openGL lowers to bzy_glsurface_open guarded by io_check; the GlSurface
	   methods lower to their bzy_glsurface_* calls. */
	emit("void main() { GlSurface g; g = Graphics.openGL(320, 240, \"t\"); long e; e = g.pollEvent(); g.swapBuffers(); bool o; o = g.isOpen(); g.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_glsurface_open") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_io_check") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_glsurface_poll") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_glsurface_swap") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_glsurface_isopen") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_glsurface_close") != NULL, 1);
}

static void test_mappedfile_lowers(void)
{
	/* FileChannel.map lowers to bzy_mmap_map guarded by io_check; the MappedFile
	   methods lower to their bzy_mmap_* calls. */
	emit("void main() { FileChannel c; c = File.openChannel(\"t\"); MappedFile m; m = c.mmap(); long n; n = m.size(); int b; b = m.getByte(0); m.putLong(0, 7); byte[] d; d = new byte[8]; m.copyInto(d, 0, 8); m.flush(); m.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_mmap_map") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_io_check") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_mmap_size") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_mmap_get_byte") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_mmap_put_long") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_mmap_copy_into") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_mmap_flush") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_mmap_close") != NULL, 1);
}

static void test_pqueue_lowers(void)
{
	/* PriorityQueue construction lowers to bzy_pq_new; the methods to bzy_pq_*. */
	emit("void main() { PriorityQueue<long> q; q = new PriorityQueue<long>(); q.add(5); long x; x = q.poll(); long p; p = q.peek(); int n; n = q.size(); bool e; e = q.isEmpty(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_pq_new") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_pq_add") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_pq_poll") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_pq_peek") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_pq_size") != NULL, 1);
}

static void test_lambda_lowers_to_closure_object(void)
{
	/* A capturing lambda emits a body label, allocates the closure object, stores
	   the code pointer at +24, and seeds the captured value from the env (+32). */
	emit("void main() { int base; base = 10; (int)->int f; f = x => x + base; }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "__lambda_0:") != NULL, 1);          /* body label. */
	ASSERT_INT(strstr(g_asm, "bzy_alloc") != NULL, 1);            /* env allocation. */
	ASSERT_INT(strstr(g_asm, "[rax + 24], rcx") != NULL, 1);      /* code pointer slot. */
	ASSERT_INT(strstr(g_asm, "[rax + 32]") != NULL, 1);           /* capture seeded from env. */
}

static void test_function_value_call_lowers(void)
{
	/* Calling a function value loads the code pointer from [closure+24] and calls
	   through it, the closure passed as the hidden arg0. */
	emit("void main() { (int)->int f; f = x => x * 2; print(f(21)); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "[rax + 24]") != NULL, 1);   /* code pointer load. */
	ASSERT_INT(strstr(g_asm, "call rax") != NULL, 1);     /* indirect call. */
}

static void test_noncapturing_singleton_and_func_value(void)
{
	/* A named function assigned to a (P)->R slot becomes a function value via a
	   synthesized adapter that calls the named function. */
	emit("int dbl(int x){ return x*2; } void main(){ (int)->int f; f = dbl; print(f(4)); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "call bzy_dbl") != NULL, 1);      /* the adapter calls the named function. */

	/* A non-capturing lambda is allocated once into a static singleton slot. */
	emit("void main(){ (int)->int f; f = x => x + 1; print(f(2)); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "__lambda_0:") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "_single") != NULL, 1);           /* cached, not re-allocated. */
}

static void test_xml_parse_lowers(void)
{
	emit("void main(){ XmlNode r = Xml.parse(\"<a/>\"); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_xml_parse") != NULL, 1);   /* the parse call. */
	ASSERT_INT(strstr(g_asm, "bzy_xml_check") != NULL, 1);   /* malformed -> XmlException. */
}

static void test_json_parse_lowers(void)
{
	emit("void main(){ JsonValue v = Json.parse(\"[1,2]\"); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_json_parse") != NULL, 1);   /* The parse call. */
	ASSERT_INT(strstr(g_asm, "bzy_json_check") != NULL, 1);   /* malformed -> JsonException */
}

static void test_http_read_request_lowers(void)
{
	emit("void main(){ Listener l = Network.listen(9999); Socket c = l.accept(); HttpRequest r = Http.readRequest(c); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_http_read_request") != NULL, 1);   /* The read call. */
	ASSERT_INT(strstr(g_asm, "bzy_http_check") != NULL, 1);          /* malformed -> HttpException */
}

static void test_pg_connect_lowers(void)
{
	emit("void main(){ PgConnection c = Postgres.connect(\"h\",5432,\"u\",\"p\",\"d\"); c.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_pg_connect") != NULL, 1);   /* The connect call. */
	ASSERT_INT(strstr(g_asm, "bzy_db_check") != NULL, 1);     /* connect failure -> DbException */
	ASSERT_INT(strstr(g_asm, "bzy_pg_close") != NULL, 1);     /* close lowering. */
}

static void test_my_connect_lowers(void)
{
	emit("void main(){ MyConnection c = Mysql.connect(\"h\",3306,\"u\",\"p\",\"d\"); c.close(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_my_connect") != NULL, 1);   /* The connect call. */
	ASSERT_INT(strstr(g_asm, "bzy_db_check") != NULL, 1);     /* connect failure -> DbException */
	ASSERT_INT(strstr(g_asm, "bzy_my_close") != NULL, 1);     /* close lowering. */
}

static void test_pg_query_lowers(void)
{
	emit("void main(){ PgConnection c = Postgres.connect(\"h\",5432,\"u\",\"p\",\"d\");"
		 " DbResult r = Postgres.query(c, \"select 1\"); Row w = r.row(0);"
		 " print(w.getLong(0)); print(w.getString(\"name\")); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_pg_query") != NULL, 1);            /* The query call. */
	ASSERT_INT(strstr(g_asm, "bzy_db_row") != NULL, 1);             /* row(i). */
	ASSERT_INT(strstr(g_asm, "bzy_db_get_long") != NULL, 1);        /* getLong(int) by index. */
	ASSERT_INT(strstr(g_asm, "bzy_db_get_string_named") != NULL, 1);/* getString(string) by name. */
}

static void test_my_query_lowers(void)
{
	emit("void main(){ MyConnection c = Mysql.connect(\"h\",3306,\"u\",\"p\",\"d\");"
		 " DbResult r = Mysql.query(c, \"select 1\"); Row w = r.row(0); print(w.getLong(0)); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_my_query") != NULL, 1);   /* The query call. */
	ASSERT_INT(strstr(g_asm, "bzy_db_row") != NULL, 1);     /* row(i) (shared accessor). */
}

static void test_my_query_params_lowers(void)
{
	emit("void main(){ MyConnection c = Mysql.connect(\"h\",3306,\"u\",\"p\",\"d\");"
		 " string[] a = new string[1]; a[0] = \"x\";"
		 " DbResult r = Mysql.query(c, \"select ?\", a); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_my_query_params") != NULL, 1);   /* The 3-arg overload. */
}

static void test_pg_query_params_lowers(void)
{
	emit("void main(){ PgConnection c = Postgres.connect(\"h\",5432,\"u\",\"p\",\"d\");"
		 " string[] a = new string[1]; a[0] = \"x\";"
		 " DbResult r = Postgres.query(c, \"select $1\", a); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_pg_query_params") != NULL, 1);   /* The 3-arg overload. */
}

static void test_xml_fields_lower(void)
{
	emit("void main(){ XmlNode r = Xml.parse(\"<a x='1'>hi</a>\"); print(r.name); print(r.text); print(r.attrCount); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "+ 24]") != NULL, 1);   /* name@24 read. */
	ASSERT_INT(strstr(g_asm, "+ 56]") != NULL, 1);   /* attrCount@56 read. */
}

static void test_combinator_lowers_to_loop(void)
{
	/* A value map presizes a fresh result List and fills it by index (the lambda
	   body is inlined, not called through a closure). */
	emit("void main(){ List<int> n = new List<int>(); n.add(1); int b = 2;"
		 " List<int> m = n.map(x => x + b); print(m.get(0)); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_vec_new") != NULL, 1);          /* result List. */
	ASSERT_INT(strstr(g_asm, "bzy_vec_reserve") != NULL, 1);      /* presized fill. */

	/* A statement-level reduce desugars to a foreach accumulation: no closure object
	   is constructed or called, so no lambda body is emitted. */
	emit("void main(){ List<int> n = new List<int>(); n.add(1);"
		 " int s = n.reduce(0, (acc, x) => acc + x); print(s); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "__lambda_") == NULL, 1);
}

static void test_treemap_lowers(void)
{
	/* TreeMap construction lowers to bzy_btree_new; the methods to bzy_btree_*. */
	emit("void main() { TreeMap<long,long> m; m = new TreeMap<long,long>(); m.put(3, 30);"
		 " long v; v = m.get(3); bool c; c = m.containsKey(3); int n; n = m.size();"
		 " long f; f = m.firstKey(); long g; g = m.floorKey(2); long[] ks; ks = m.getKeys(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_btree_new") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_put") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_get") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_has") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_size") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_first") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_floor") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_keys") != NULL, 1);

	/* TreeSet add lowers to put (value-less); contains/first to btree entries. */
	emit("void main() { TreeSet<long> s; s = new TreeSet<long>(); s.add(5); long f; f = s.first(); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_btree_new") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_put") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_btree_first") != NULL, 1);
}

static void test_dynamic_extern_lowers(void)
{
	/* A dynamic extern call lowers to a bzy_dynsym resolve, a per-extern slot, an
	   io_check, and an indirect call through the slot; Ffi.bind -> bzy_ffi_bind. */
	emit("extern dynamic int abs(int n);"
		 " void main() { Ffi.bind(\"libc.so.6\"); int x; x = abs(-5); print(x); }", TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "bzy_dynsym") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "__dynslot_abs") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_io_check") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "bzy_ffi_bind") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "call qword [rel __dynslot_abs]") != NULL, 1);
}

static void test_ffi_variadic_win64_fpdup(void)
{
	/* SP3 Win64 variadic ABI: a double vararg is duplicated into its GP register
	   (movq) alongside the xmm register. */
	emit("extern int printf(string fmt, ...);"
		 " void main() { printf(\"%f\", 1.5); }", TARGET_WINDOWS);
	ASSERT_INT(strstr(g_asm, "movq") != NULL, 1);
}

static void test_simd_vectorize_int_add(void)
{
	/* The vectorizer lives on the IR region path; main() forces BZY_IR=0 for the
	   emitter assertions, so enable the IR backend for this case and restore it. */
	putenv("BZY_IR=1");
	bzy_config_reset_cache();

	/* A counted, unit-stride int[] element-wise add with BCE-proved indices
	   lowers to a packed body (movdqu + paddd) on the IR region path. */
	emit("void main() {"
		 " int[] a; a = new int[1023];"
		 " int[] b; b = new int[1023];"
		 " int[] c; c = new int[1023];"
		 " for (int i = 0; i < 1023; i = i + 1) { a[i] = i; b[i] = i + 1; }"
		 " for (int i = 0; i < 1023; i = i + 1) { c[i] = a[i] + b[i]; } }",
		 TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "paddd") != NULL, 1);
	ASSERT_INT(strstr(g_asm, "movdqu") != NULL, 1);

	/* A carried dependence (c[i] = c[i-1] + a[i]) must NOT vectorize. */
	emit("void main() {"
		 " int[] a; a = new int[1023];"
		 " int[] c; c = new int[1023];"
		 " for (int i = 0; i < 1023; i = i + 1) { a[i] = i; }"
		 " c[0] = 0;"
		 " for (int i = 1; i < 1023; i = i + 1) { c[i] = c[i - 1] + a[i]; } }",
		 TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "paddd") == NULL, 1);

	/* A long[] (8-byte) element loop must NOT vectorize: the gate is int-only. */
	emit("void main() {"
		 " long[] a; a = new long[1023];"
		 " long[] b; b = new long[1023];"
		 " long[] c; c = new long[1023];"
		 " for (int i = 0; i < 1023; i = i + 1) { a[i] = i; b[i] = i + 1; }"
		 " for (int i = 0; i < 1023; i = i + 1) { c[i] = a[i] + b[i]; } }",
		 TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "paddd") == NULL, 1);

	/* A multiply loop lowers to pmulld (the int-only 32-bit packed multiply). */
	emit("void main() {"
		 " int[] a; a = new int[1023];"
		 " int[] b; b = new int[1023];"
		 " int[] c; c = new int[1023];"
		 " for (int i = 0; i < 1023; i = i + 1) { a[i] = i; b[i] = i + 1; }"
		 " for (int i = 0; i < 1023; i = i + 1) { c[i] = a[i] * b[i]; } }",
		 TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "pmulld") != NULL, 1);

	/* A foreign-index stencil (reads a[i+1], not the induction index) must NOT
	   vectorize. */
	emit("void main() {"
		 " int[] a; a = new int[1023];"
		 " int[] d; d = new int[1023];"
		 " for (int i = 0; i < 1023; i = i + 1) { a[i] = i; }"
		 " for (int i = 0; i < 1022; i = i + 1) { d[i] = a[i] + a[i + 1]; } }",
		 TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "paddd") == NULL, 1);
	ASSERT_INT(strstr(g_asm, "movdqu") == NULL, 1);

	/* A non-unit stride (i += 2) must NOT vectorize. */
	emit("void main() {"
		 " int[] a; a = new int[1024];"
		 " int[] b; b = new int[1024];"
		 " int[] c; c = new int[1024];"
		 " for (int i = 0; i < 1024; i = i + 1) { a[i] = i; b[i] = i + 1; }"
		 " for (int i = 0; i < 1024; i = i + 2) { c[i] = a[i] + b[i]; } }",
		 TARGET_LINUX);
	ASSERT_INT(strstr(g_asm, "paddd") == NULL, 1);

	/* Restore the emitter-forcing flag for the remaining cases. */
	putenv("BZY_IR=0");
	bzy_config_reset_cache();
}

int main(void)
{
	/* These assertions check the EMITTER's output specifically; force it on even
	   now that the IR backend is the default for eligible functions. */
	putenv("BZY_IR=0");

	RUN(test_inline_alloc_windows_pool_pop);
	RUN(test_inline_arc_fast_paths);
	RUN(test_array_elem_stride);
	RUN(test_loop_rotation_bottom_test);
	RUN(test_nonneg_division_drops_sign_bias);
	RUN(test_register_operand_compare);
	RUN(test_inplace_register_arithmetic);
	RUN(test_promotion_register_resident);
	RUN(test_float_promotion_xmm);
	RUN(test_fp_indexed_load_direct_addressing);
	RUN(test_loop_scoped_float_promotion);
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
	RUN(test_mul_strength_reduction);
	RUN(test_fp_leaf_fusion);
	RUN(test_inplace_mac);
	RUN(test_register_index_addr);
	RUN(test_boundscheck_cse_repeated_index);
	RUN(test_foreach_base_hoist);
	RUN(test_map_foreach_snapshot_handle);
	RUN(test_shared_set_closure);
	RUN(test_shared_array_gating);
	RUN(test_vec_gating_and_store_barriers);
	RUN(test_subclass_declared_before_parent);
	RUN(test_map_compound_lowering);
	RUN(test_div_strength_reduction);
	RUN(test_branch_fusion);
	RUN(test_divisibility_test);
	RUN(test_unroll_folds_derived_constant);
	RUN(test_nested_unroll_outer_constant_survives_inner);
	RUN(test_unroll_kill_not_resurrected_by_inner_exit);
	RUN(test_unroll_constant_index_direct_disp);
	RUN(test_unrolled_mac_memory_operand_imul);
	RUN(test_desktop_is_enabled_lowers);
	RUN(test_desktop_button_listener_lowers);
	RUN(test_overload_encode);
	RUN(test_overload_select_exact_beats_widening);
	RUN(test_overload_select_null_ambiguous);
	RUN(test_overload_set_ambiguous);
	RUN(test_grow_ensure);
	RUN(test_ffi_fromcstring_lowers);
	RUN(test_ffi_frombytes_lowers);
	RUN(test_ffi_array_arg_marshals_data_ptr);
	RUN(test_ffi_blocking_five_args);
	RUN(test_ffi_str_tobytes_lowers);
	RUN(test_ffi_frombytes_arr_lowers);
	RUN(test_ffi_callback_type_lowers);
	RUN(test_ffi_variadic_parses);
	RUN(test_ffi_variadic_win64_fpdup);
	RUN(test_simd_vectorize_int_add);
	RUN(test_ffi_getenv_lowers);
	RUN(test_ffi_await_shutdown_lowers);
	RUN(test_system_realtime_io_lowers);
	RUN(test_filechannel_lock_lowers);
	RUN(test_raw_socket_lowers);
	RUN(test_tls_lowers);
	RUN(test_dtls_lowers);
	RUN(test_surface_lowers);
	RUN(test_glsurface_lowers);
	RUN(test_mappedfile_lowers);
	RUN(test_pqueue_lowers);
	RUN(test_lambda_lowers_to_closure_object);
	RUN(test_function_value_call_lowers);
	RUN(test_noncapturing_singleton_and_func_value);
	RUN(test_combinator_lowers_to_loop);
	RUN(test_xml_parse_lowers);
	RUN(test_json_parse_lowers);
	RUN(test_http_read_request_lowers);
	RUN(test_pg_connect_lowers);
	RUN(test_my_connect_lowers);
	RUN(test_pg_query_lowers);
	RUN(test_my_query_lowers);
	RUN(test_my_query_params_lowers);
	RUN(test_pg_query_params_lowers);
	RUN(test_xml_fields_lower);
	RUN(test_treemap_lowers);
	RUN(test_dynamic_extern_lowers);
	RUN(test_stack_args_caller);
	RUN(test_stack_args_callee);
	SUMMARY();
	return 0;
}
