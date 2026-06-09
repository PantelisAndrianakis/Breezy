CC      = gcc

# Host detection: pick the coroutine backend and platform libs/flags per OS.
# Windows (MinGW, uname = MINGW*/MSYS*) uses fibers + a 64 MB stack reserve at
# link time (--stack is a PE-only ld flag). Linux uses ucontext + pthreads.
UNAME := $(shell uname -s)
ifeq ($(findstring Linux,$(UNAME)),Linux)
  CORO_SRC      = runtime/coroutine.c
  PLATFORM_LIBS = -lpthread
  STACKFLAG     =
  PLATFORM_DEFS = -D_GNU_SOURCE   # Expose POSIX (clock_gettime/sem_timedwait) + GNU (accept4/SOCK_NONBLOCK/MSG_NOSIGNAL) under -std=c99.
  OBJDIR        = build/linux
else
  CORO_SRC      = runtime/coroutine.c
  PLATFORM_LIBS =
  STACKFLAG     = -Wl,--stack,0x4000000
  PLATFORM_DEFS =
  OBJDIR        = build/win
endif

CFLAGS  = -std=c99 -Wall -Wextra -g -Isrc $(STACKFLAG) $(PLATFORM_DEFS)

# The runtime archive (lib_breezy.a) is statically linked into every compiled
# Breezy program, so its allocator, ARC, scheduler, channel, and I/O hot paths
# are always built optimized (-O2) regardless of debug/release - shipping the
# runtime at -O0 would tax every user binary. Runtime debugging is unaffected:
# the test_runtime/test_coroutine targets compile the sources directly with the
# debug CFLAGS; only the archive objects use these flags.
RT_CFLAGS = -std=c99 -Wall -Wextra -O2 -Iruntime $(PLATFORM_DEFS)

# Release flags: optimize, drop debug info, and let the linker garbage-collect
# unreferenced functions so stages a binary never calls are not carried along.
# -s strips the symbol table at link time (the bulk of the debug-build size).
# Note: -fdata-sections is deliberately omitted. On the MinGW/PE target it
# emits each global into its own named section, which the linker materializes
# as initialized .data on disk -- that would force the ~1.6 MB zero-initialized
# g_nodes pool out of .bss and bloat the binary instead of shrinking it.
# The 64 MB stack reserve is still required because the TypeTable lives on the
# stack in the driver.
RELEASE_CFLAGS = -std=c99 -Wall -Wextra -O2 -Isrc \
                 -ffunction-sections -Wl,--gc-sections \
                 -s $(STACKFLAG)

OBJS    = src/lexer.c src/ast.c src/parser.c src/enums.c src/generics.c src/types.c \
          src/resolve.c src/symtable.c src/codegen.c src/ownership.c src/escape.c src/constprop.c src/promote.c src/nonneg.c src/bce.c src/prelude.c src/config.c

# Runtime modules, per host. Common = portable (compute + concurrency core);
# each host then adds its own I/O backends — Windows uses IOCP + WinHTTP +
# CreateProcess, Linux uses an epoll reactor + libcurl + fork/exec. The process
# shell (system.c) now builds on both.
RT_COMMON = alloc print string strconv array map vector clock random exception regex \
            map_entry channel timer reflect args scheduler pollstate
ifeq ($(findstring Linux,$(UNAME)),Linux)
  RT_NAMES = $(RT_COMMON) coroutine offload system file filechannel logger reactor_epoll socket udp http
else
  RT_NAMES = $(RT_COMMON) coroutine file offload system iocp socket udp filechannel logger http
endif
RT_SRC  = $(addprefix runtime/,$(addsuffix .c,$(RT_NAMES)))
# Objects and the runtime archive live in a per-host build dir (build/win or
# build/linux) so Windows and Linux artifacts never collide in a shared tree.
RT_OBJ  = $(addprefix $(OBJDIR)/,$(addsuffix .o,$(RT_NAMES))) $(OBJDIR)/entry.o
RT_HDR  = runtime/breezy.h runtime/coroutine.h runtime/platform.h runtime/network_internal.h
LIB     = $(OBJDIR)/lib_breezy.a

.PHONY: all clean test integration release

all: breezy

breezy: src/main.c $(OBJS) $(LIB)
	$(CC) $(CFLAGS) -o breezy src/main.c $(OBJS)

# Stripped, garbage-collected build of the compiler. Produces the same
# 'breezy' binary as 'all' but a fraction of the size (no debug symbols,
# unused functions removed). Use this for distribution.
release: src/main.c $(OBJS) $(LIB)
	$(CC) $(RELEASE_CFLAGS) -o breezy src/main.c $(OBJS)

# Test binaries also build into the per-host dir, keeping the project root clean.
$(OBJDIR)/test_lexer: tests/test_lexer.c src/lexer.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_lexer.c src/lexer.c

$(OBJDIR)/test_ast: tests/test_ast.c src/ast.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_ast.c src/ast.c

$(OBJDIR)/test_config: tests/test_config.c src/config.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_config.c src/config.c

$(OBJDIR)/test_codegen: tests/test_codegen.c $(OBJS) | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_codegen.c $(OBJS)

$(OBJDIR)/test_coroutine: tests/test_coroutine.c $(CORO_SRC) runtime/coroutine.h | $(OBJDIR)
	$(CC) $(CFLAGS) -Iruntime -Itests -o $@ tests/test_coroutine.c $(CORO_SRC) $(PLATFORM_LIBS)

$(OBJDIR)/test_reactor: tests/test_reactor.c runtime/pollstate.c runtime/pollstate.h | $(OBJDIR)
	$(CC) $(CFLAGS) -Iruntime -o $@ tests/test_reactor.c runtime/pollstate.c $(PLATFORM_LIBS)

$(OBJDIR)/test_parser: tests/test_parser.c src/lexer.c src/ast.c src/parser.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_parser.c src/lexer.c src/ast.c src/parser.c

$(OBJDIR)/test_types: tests/test_types.c src/lexer.c src/ast.c src/parser.c src/types.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_types.c src/lexer.c src/ast.c src/parser.c src/types.c

$(OBJDIR)/test_resolve: tests/test_resolve.c $(OBJS) | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_resolve.c $(OBJS)

$(OBJDIR)/test_promote: tests/test_promote.c $(OBJS) | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_promote.c $(OBJS)

$(OBJDIR)/test_ownership: tests/test_ownership.c $(OBJS) | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_ownership.c $(OBJS)

$(OBJDIR)/test_escape: tests/test_escape.c $(OBJS) | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_escape.c $(OBJS)

# Each runtime object compiles into the per-host build dir (host-selected set in
# RT_OBJ), so Windows and Linux objects never collide in one tree.
$(OBJDIR)/%.o: runtime/%.c $(RT_HDR) | $(OBJDIR)
	$(CC) $(RT_CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(LIB): $(RT_OBJ)
	rm -f $(LIB)   # Rebuild from scratch so renamed/removed members never linger in the archive.
	ar rcs $(LIB) $(RT_OBJ)

$(OBJDIR)/test_runtime: tests/test_runtime.c $(RT_SRC) $(RT_HDR) | $(OBJDIR)
	$(CC) $(CFLAGS) -Iruntime -o $@ tests/test_runtime.c $(RT_SRC) -lws2_32 -lwinhttp

TEST_BINS = $(addprefix $(OBJDIR)/,test_lexer test_ast test_config test_parser test_types \
            test_resolve test_ownership test_escape test_promote test_codegen test_coroutine test_reactor test_runtime)

test: $(TEST_BINS) breezy
	$(OBJDIR)/test_lexer
	$(OBJDIR)/test_ast
	$(OBJDIR)/test_config
	$(OBJDIR)/test_parser
	$(OBJDIR)/test_types
	$(OBJDIR)/test_resolve
	$(OBJDIR)/test_ownership
	$(OBJDIR)/test_escape
	$(OBJDIR)/test_promote
	$(OBJDIR)/test_codegen
	$(OBJDIR)/test_coroutine
	$(OBJDIR)/test_reactor
	$(OBJDIR)/test_runtime
	bash tests/run_integration.sh

integration: breezy $(LIB)
	bash tests/run_integration.sh

clean:
	rm -rf build
	rm -f breezy breezy.exe test_lexer test_ast test_config test_codegen test_coroutine test_parser test_types test_resolve test_ownership test_escape test_runtime lib_breezy.a runtime/*.o *.o src/*.o out.asm out.obj out.exe out_cg_test.asm out_elf.o
