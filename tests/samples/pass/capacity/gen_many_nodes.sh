#!/bin/sh
# Emit a main() with N trivial statements; each "x = x + 1;" is several AST nodes.
N="${1:-60000}"
printf 'void main()\n{\n\tlong x;\n\tx = 0;\n'
i=0
while [ "$i" -lt "$N" ]; do printf '\tx = x + 1;\n'; i=$((i+1)); done
printf '\tprint(x);\n}\n'
