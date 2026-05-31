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
check_fail() {
    local name="$1" target="$2"
    ./breezy "$target" >/dev/null 2>&1
    if [ $? -ne 0 ]; then echo "  $name: OK (rejected)"
    else echo "  $name: FAIL (compiled, expected rejection)"; fail=1; fi
}
echo "Integration tests"
check minimal     tests/samples/minimal.bzy    "0"
check arith       tests/samples/arith.bzy      "14"
check if_else     tests/samples/if_else.bzy    "1"
check while       tests/samples/while.bzy      "10"
check multi_fn    tests/samples/multi_fn.bzy   "42"
check inheritance tests/samples/proj_inherit  "2"
check leak        tests/samples/proj_leak      "1"
check cycle       tests/samples/proj_cycle     "0"
check_fail narrow_no_cast tests/samples/bad_narrow.bzy
check_fail mixed_sign     tests/samples/bad_mixed_sign.bzy
check_fail int_condition  tests/samples/bad_int_cond.bzy
check_fail bool_int_cast  tests/samples/bad_bool_cast.bzy
if [ $fail -eq 0 ]; then echo "All integration tests passed"; else echo "FAILURES"; exit 1; fi
