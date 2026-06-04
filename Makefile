CC      = gcc

# Host detection: pick the coroutine backend and platform libs/flags per OS.
# Windows (MinGW, uname = MINGW*/MSYS*) uses fibers + a 64 MB stack reserve at
# link time (--stack is a PE-only ld flag). Linux uses ucontext + pthreads.
UNAME := $(shell uname -s)
ifeq ($(findstring Linux,$(UNAME)),Linux)
  CORO_SRC      = runtime/coroutine_posix.c
  PLATFORM_LIBS = -lpthread
  STACKFLAG     =
  PLATFORM_DEFS = -D_GNU_SOURCE   # Expose POSIX (clock_gettime/sem_timedwait) + GNU (accept4/SOCK_NONBLOCK/MSG_NOSIGNAL) under -std=c99.
else
  CORO_SRC      = runtime/coroutine_win.c
  PLATFORM_LIBS =
  STACKFLAG     = -Wl,--stack,0x4000000
  PLATFORM_DEFS =
endif

CFLAGS  = -std=c99 -Wall -Wextra -g -Isrc $(STACKFLAG) $(PLATFORM_DEFS)

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
                 -s -Wl,--stack,0x4000000

OBJS    = src/lexer.c src/ast.c src/parser.c src/generics.c src/types.c \
          src/resolve.c src/symtable.c src/codegen.c src/ownership.c src/escape.c src/prelude.c src/config.c

# Runtime modules, per host. Common = portable (compute + concurrency core);
# the I/O backends (files, sockets, IOCP, HTTP, offload, process shell) are
# Windows-only until the Linux ports land (Parts 8-3/8-4). Linux substitutes
# ucontext coroutines + I/O inflight stubs for the scheduler's deadlock gate.
RT_COMMON = alloc print string array map vector clock random exception regex \
            map_entry channel timer reflect args scheduler
ifeq ($(findstring Linux,$(UNAME)),Linux)
  RT_NAMES = $(RT_COMMON) coroutine_posix offload file filechannel logger reactor_epoll socket udp http
else
  RT_NAMES = $(RT_COMMON) coroutine_win file offload system iocp socket udp filechannel logger http
endif
RT_SRC  = $(addprefix runtime/,$(addsuffix .c,$(RT_NAMES)))
RT_OBJ  = $(addprefix runtime/,$(addsuffix .o,$(RT_NAMES))) runtime/entry.o
RT_HDR  = runtime/breezy.h runtime/coroutine.h runtime/platform.h runtime/network_internal.h

.PHONY: all clean test integration release

all: breezy

breezy: src/main.c $(OBJS) lib_breezy.a
	$(CC) $(CFLAGS) -o breezy src/main.c $(OBJS)

# Stripped, garbage-collected build of the compiler. Produces the same
# 'breezy' binary as 'all' but a fraction of the size (no debug symbols,
# unused functions removed). Use this for distribution.
release: src/main.c $(OBJS) lib_breezy.a
	$(CC) $(RELEASE_CFLAGS) -o breezy src/main.c $(OBJS)

test_lexer: tests/test_lexer.c src/lexer.c
	$(CC) $(CFLAGS) -o test_lexer tests/test_lexer.c src/lexer.c

test_ast: tests/test_ast.c src/ast.c
	$(CC) $(CFLAGS) -o test_ast tests/test_ast.c src/ast.c

test_config: tests/test_config.c src/config.c
	$(CC) $(CFLAGS) -o test_config tests/test_config.c src/config.c

test_codegen: tests/test_codegen.c $(OBJS)
	$(CC) $(CFLAGS) -o test_codegen tests/test_codegen.c $(OBJS)

test_coroutine: tests/test_coroutine.c $(CORO_SRC) runtime/coroutine.h
	$(CC) $(CFLAGS) -Iruntime -Itests -o test_coroutine tests/test_coroutine.c $(CORO_SRC) $(PLATFORM_LIBS)

test_parser: tests/test_parser.c src/lexer.c src/ast.c src/parser.c
	$(CC) $(CFLAGS) -o test_parser tests/test_parser.c src/lexer.c src/ast.c src/parser.c

test_types: tests/test_types.c src/lexer.c src/ast.c src/parser.c src/types.c
	$(CC) $(CFLAGS) -o test_types tests/test_types.c src/lexer.c src/ast.c src/parser.c src/types.c

test_resolve: tests/test_resolve.c $(OBJS)
	$(CC) $(CFLAGS) -o test_resolve tests/test_resolve.c $(OBJS)

test_ownership: tests/test_ownership.c $(OBJS)
	$(CC) $(CFLAGS) -o test_ownership tests/test_ownership.c $(OBJS)

test_escape: tests/test_escape.c $(OBJS)
	$(CC) $(CFLAGS) -o test_escape tests/test_escape.c $(OBJS)

# Each runtime object from its source (host-selected set in RT_OBJ).
runtime/%.o: runtime/%.c $(RT_HDR)
	$(CC) $(CFLAGS) -Iruntime -c $< -o $@

lib_breezy.a: $(RT_OBJ)
	rm -f lib_breezy.a   # Rebuild from scratch so renamed/removed members never linger in the archive.
	ar rcs lib_breezy.a $(RT_OBJ)

test_runtime: tests/test_runtime.c $(RT_SRC) $(RT_HDR)
	$(CC) $(CFLAGS) -Iruntime -o test_runtime tests/test_runtime.c $(RT_SRC) -lws2_32 -lwinhttp

test: test_lexer test_ast test_config test_parser test_types test_resolve test_ownership test_escape test_codegen test_coroutine test_runtime breezy
	./test_lexer
	./test_ast
	./test_config
	./test_parser
	./test_types
	./test_resolve
	./test_ownership
	./test_escape
	./test_codegen
	./test_coroutine
	./test_runtime
	bash tests/run_integration.sh

integration: breezy lib_breezy.a
	bash tests/run_integration.sh

clean:
	rm -f breezy test_lexer test_ast test_config test_codegen test_coroutine test_parser test_types test_resolve test_ownership test_escape test_runtime lib_breezy.a runtime/*.o *.o src/*.o out.asm out.obj out.exe out_cg_test.asm
