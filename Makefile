CC      = gcc
# The TypeTable is ~8 MB and is declared as a stack local in the driver and
# tests, which overflows Windows' default ~1 MB main-thread stack. Reserve a
# 64 MB stack at link time so those binaries run.
CFLAGS  = -std=c99 -Wall -Wextra -g -Isrc -Wl,--stack,0x4000000

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

OBJS    = src/lexer.c src/ast.c src/parser.c src/types.c \
          src/resolve.c src/symtable.c src/codegen.c src/ownership.c src/escape.c src/prelude.c

RT_SRC  = runtime/alloc.c runtime/print.c runtime/string.c runtime/array.c runtime/map.c runtime/vector.c runtime/clock.c runtime/random.c runtime/exception.c runtime/regex.c runtime/coroutine_win.c runtime/scheduler.c runtime/map_entry.c runtime/channel.c runtime/file.c runtime/timer.c runtime/offload.c
RT_HDR  = runtime/breezy.h runtime/coroutine.h

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

lib_breezy.a: $(RT_SRC) $(RT_HDR)
	rm -f lib_breezy.a   # Rebuild from scratch so renamed/removed members never linger in the archive.
	$(CC) $(CFLAGS) -Iruntime -c runtime/alloc.c -o runtime/alloc.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/print.c -o runtime/print.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/string.c -o runtime/string.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/array.c -o runtime/array.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/map.c -o runtime/map.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/vector.c -o runtime/vector.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/clock.c -o runtime/clock.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/random.c -o runtime/random.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/exception.c -o runtime/exception.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/regex.c -o runtime/regex.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/coroutine_win.c -o runtime/coroutine_win.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/scheduler.c -o runtime/scheduler.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/map_entry.c -o runtime/map_entry.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/channel.c -o runtime/channel.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/file.c -o runtime/file.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/timer.c -o runtime/timer.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/offload.c -o runtime/offload.o
	$(CC) $(CFLAGS) -Iruntime -c runtime/entry.c -o runtime/entry.o
	ar rcs lib_breezy.a runtime/alloc.o runtime/print.o runtime/string.o runtime/array.o runtime/map.o runtime/vector.o runtime/clock.o runtime/random.o runtime/exception.o runtime/regex.o runtime/coroutine_win.o runtime/scheduler.o runtime/map_entry.o runtime/channel.o runtime/file.o runtime/timer.o runtime/offload.o runtime/entry.o

test_runtime: tests/test_runtime.c $(RT_SRC) $(RT_HDR)
	$(CC) $(CFLAGS) -Iruntime -o test_runtime tests/test_runtime.c $(RT_SRC)

test: test_lexer test_ast test_parser test_types test_resolve test_ownership test_escape test_runtime breezy
	./test_lexer
	./test_ast
	./test_parser
	./test_types
	./test_resolve
	./test_ownership
	./test_escape
	./test_runtime
	bash tests/run_integration.sh

integration: breezy lib_breezy.a
	bash tests/run_integration.sh

clean:
	rm -f breezy test_lexer test_ast test_parser test_types test_resolve test_ownership test_escape test_runtime lib_breezy.a runtime/*.o *.o src/*.o out.asm out.obj out.exe
