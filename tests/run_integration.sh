#!/usr/bin/env bash
set -u
fail=0
check() {
    local name="$1" target="$2" expected="$3"
    ./breezy "$target" >/dev/null 2>&1
    if [ $? -ne 0 ]; then echo "  $name: COMPILE FAILED"; fail=1; return; fi
    local got; got="$(./out.exe)"
    got="${got//$'\r'/}"   # Normalize Windows CRLF line endings to LF.
    if [ "$got" == "$expected" ]; then echo "  $name: OK"
    else echo "  $name: FAIL (expected '$expected', got '$got')"; fail=1; fi
}
check_fail() {
    local name="$1" target="$2"
    ./breezy "$target" >/dev/null 2>&1
    if [ $? -ne 0 ]; then echo "  $name: OK (rejected)"
    else echo "  $name: FAIL (compiled, expected rejection)"; fail=1; fi
}
check_abort() {
    local name="$1" target="$2"
    ./breezy "$target" >/dev/null 2>&1 || { echo "  $name: COMPILE FAILED"; fail=1; return; }
    ./out.exe >/dev/null 2>&1
    if [ $? -ne 0 ]; then echo "  $name: OK (aborted)"
    else echo "  $name: FAIL (no abort)"; fail=1; fi
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
check scalars     tests/samples/proj_scalars   $'0\n0\n5000000000\n4000000000\ntrue\n-1\nfalse\ntrue'
check floats      tests/samples/proj_floats    $'3.75\n3.375\n7\ntrue\n10\n1.5'
check floats_fn   tests/samples/proj_floats_fn $'10'
check string      tests/samples/proj_string    $'Hello, Breezy\n13'
check array       tests/samples/proj_array     $'30'
check array_obj   tests/samples/proj_array_obj $'4\n5'
check array_cycle tests/samples/proj_array_cycle $'0'
check_abort array_oob tests/samples/bad_array_oob.bzy
check stringbuilder tests/samples/proj_sb        $'ababab'
check map         tests/samples/proj_map       $'11\n2\nfalse\n2'
check map_obj     tests/samples/proj_map_obj   $'6\n9'
check map_cycle   tests/samples/proj_map_cycle $'0'
check incdec      tests/samples/proj_incdec    $'42\n40'
check break_loop  tests/samples/proj_break     $'8'
check_fail break_outside tests/samples/bad_break_outside.bzy
check for_loop    tests/samples/proj_for       $'55\n15'
check foreach_array  tests/samples/proj_foreach_array  $'30'
check foreach_string tests/samples/proj_foreach_string $'198'
check foreach_map    tests/samples/proj_foreach_map    $'6\n3'
check_fail foreach_noniter tests/samples/bad_foreach_noniter.bzy
check box_int    tests/samples/proj_box_int    $'7\ntrue\nfalse'
check box_string tests/samples/proj_box_string $'true\nfalse'
check box_obj    tests/samples/proj_box_obj    $'2\n2'
check box_cycle  tests/samples/proj_box_cycle  $'0'
check list_int    tests/samples/proj_list_int     $'3\n99\n2\nfalse\n99'
check stack_queue tests/samples/proj_stack_queue  $'2\n2\n1\n7\n7\n8'
check deque       tests/samples/proj_deque        $'1\n3\n1\n3\n1'
check set_string  tests/samples/proj_set_string   $'2\ntrue\nfalse\nfalse\n1'
check foreach_list tests/samples/proj_foreach_list $'12'
check foreach_set  tests/samples/proj_foreach_set  $'30\n2'
check list_obj    tests/samples/proj_list_obj      $'6\n8\n7'
check list_cycle  tests/samples/proj_list_cycle    $'0'
check switch      tests/samples/proj_switch        $'20\n30\n10\n99'
check switch_dense  tests/samples/proj_switch_dense  $'100\n101\n102\n103\n104\n999'
check switch_braces tests/samples/proj_switch_braces $'2'
check block_comment tests/samples/proj_block_comment $'7'
check compound    tests/samples/proj_compound      $'6\n10\nabc'
check math_basic  tests/samples/proj_math_basic    $'4\n5\n2.5'
check math_minmax tests/samples/proj_math_minmax   $'7\n3\n5\n0\n2.5'
check math_round  tests/samples/proj_math_round    $'2\n3\n2\n3\ntrue\nfalse'
check math_libm   tests/samples/proj_math_libm     $'1024\n1\n1\n0'
check clock       tests/samples/proj_clock         $'true\ntrue'
check random      tests/samples/proj_random        $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\n4'
check_fail narrow_no_cast tests/samples/bad_narrow.bzy
check_fail mixed_sign     tests/samples/bad_mixed_sign.bzy
check_fail int_condition  tests/samples/bad_int_cond.bzy
check_fail bool_int_cast  tests/samples/bad_bool_cast.bzy
check_fail float_needs_cast tests/samples/bad_int_to_float.bzy
check_fail dbl_to_float     tests/samples/bad_double_to_float.bzy
check_fail float_int_mix    tests/samples/bad_float_int_mix.bzy
if [ $fail -eq 0 ]; then echo "All integration tests passed"; else echo "FAILURES"; exit 1; fi
