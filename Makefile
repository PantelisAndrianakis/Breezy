CC      = gcc
# The TypeTable is ~8 MB and is declared as a stack local in the driver and
# tests, which overflows Windows' default ~1 MB main-thread stack. Reserve a
# 64 MB stack at link time so those binaries run.
CFLAGS  = -std=c99 -Wall -Wextra -g -Isrc -Wl,--stack,0x4000000

OBJS    = src/lexer.c src/ast.c src/parser.c src/types.c \
          src/resolve.c src/symtable.c src/codegen.c

RT_SRC  = runtime/alloc.c
RT_HDR  = runtime/breezy.h

.PHONY: all clean test integration

all: breezy

breezy: src/main.c $(OBJS) lib_breezy.a
	$(CC) $(CFLAGS) -o breezy src/main.c $(OBJS)

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

lib_breezy.a: $(RT_SRC) $(RT_HDR)
	$(CC) $(CFLAGS) -Iruntime -c runtime/alloc.c -o runtime/alloc.o
	ar rcs lib_breezy.a runtime/alloc.o

test_runtime: tests/test_runtime.c $(RT_SRC) $(RT_HDR)
	$(CC) $(CFLAGS) -Iruntime -o test_runtime tests/test_runtime.c $(RT_SRC)

test: test_lexer test_ast test_parser test_types test_resolve test_runtime breezy
	./test_lexer
	./test_ast
	./test_parser
	./test_types
	./test_resolve
	./test_runtime
	bash tests/run_integration.sh

integration: breezy lib_breezy.a
	bash tests/run_integration.sh

clean:
	rm -f breezy test_lexer test_ast test_parser test_types test_resolve test_runtime lib_breezy.a runtime/*.o *.o src/*.o out.asm out.obj out.exe
