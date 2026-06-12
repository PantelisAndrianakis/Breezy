#!/bin/sh
# Emit a non-builtin generic Crate<T> instantiated with N distinct user types,
# exceeding the old 256 MAX_INSTANCES cap. ("Box" is a built-in template name.)
N="${1:-300}"
printf 'class Crate<T> { T v; void set(T x) { this.v = x; } T get() { return this.v; } }\n'
i=0
while [ "$i" -lt "$N" ]; do printf 'class S%d { int a; }\n' "$i"; i=$((i+1)); done
printf 'void main()\n{\n'
i=0
while [ "$i" -lt "$N" ]; do printf '\tCrate<S%d> c%d = new Crate<S%d>();\n' "$i" "$i" "$i"; i=$((i+1)); done
printf '\tprint(%d);\n}\n' "$N"
