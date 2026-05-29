#!/usr/bin/env bash
set -u
fail=0
check() {
    local name="$1" target="$2" expected="$3"
    ./breezy "$target" >/dev/null 2>&1
    if [ $? -ne 0 ]; then echo "  $name: COMPILE FAILED"; fail=1; return; fi
    local got; got="$(./out.exe)"
    if [ "$got" == "$expected" ]; then echo "  $name: OK"
    else echo "  $name: FAIL (expected '$expected', got '$got')"; fail=1; fi
}
echo "Integration tests"
check minimal     tests/samples/minimal.bz    "0"
check arith       tests/samples/arith.bz      "14"
check if_else     tests/samples/if_else.bz    "1"
check while       tests/samples/while.bz      "10"
check multi_fn    tests/samples/multi_fn.bz   "42"
check inheritance tests/samples/proj_inherit  "2"
if [ $fail -eq 0 ]; then echo "All integration tests passed"; else echo "FAILURES"; exit 1; fi
