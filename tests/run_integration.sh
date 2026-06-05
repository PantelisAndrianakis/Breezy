#!/usr/bin/env bash
# The single cross-platform integration suite: passes 163/163 on both Windows
# (MinGW) and Linux (ELF64/SysV). The Linux backend now supports files, sockets,
# HTTP, and process I/O, so there is no separate Linux subset. On Linux run it
# directly (`bash tests/run_integration.sh` or `make integration`); `make test`
# is Windows-only because test_runtime links Windows libraries.
set -u
export TZ=UTC0   # Make Clock.getDateString output deterministic across machines.
fail=0
# Compile a sample, retrying a few times: on Windows a just-run out.exe can stay
# briefly locked by the OS/AV, so gcc's link to out.exe transiently fails with a
# sharing violation. Used by the success-expecting checks (not check_fail).
bzy_build() {
    local target="$1" i
    for i in 1 2 3 4 5; do
        ./breezy "$target" >/dev/null 2>&1 && return 0
        sleep 0.3
    done
    return 1
}
check() {
    local name="$1" target="$2" expected="$3"
    bzy_build "$target"
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
    bzy_build "$target" || { echo "  $name: COMPILE FAILED"; fail=1; return; }
    ./out.exe >/dev/null 2>&1
    if [ $? -ne 0 ]; then echo "  $name: OK (aborted)"
    else echo "  $name: FAIL (no abort)"; fail=1; fi
}
check_throws() {
    local name="$1" target="$2" expect="$3"
    bzy_build "$target" || { echo "  $name: COMPILE FAILED"; fail=1; return; }
    local out; out="$(./out.exe 2>&1)"; local code=$?
    out="${out//$'\r'/}"
    if [ $code -eq 0 ]; then echo "  $name: FAIL (expected abort)"; fail=1
    elif echo "$out" | grep -qF "$expect"; then echo "  $name: OK (threw)"
    else echo "  $name: FAIL (missing '$expect' in: $out)"; fail=1; fi
}
echo "Integration tests"
check minimal     tests/samples/minimal.bzy    "0"
check arith       tests/samples/arith.bzy      "14"
check if_else     tests/samples/if_else.bzy    "1"
check while       tests/samples/while.bzy      "10"
check multi_fn    tests/samples/multi_fn.bzy   "42"
check inheritance tests/samples/proj_inherit  "2"
check ctor        tests/samples/proj_ctor      $'3
4
7'
check default_args   tests/samples/proj_default_args  $'15\n6\n1\n2\n7\n2'
check vector_zero    tests/samples/proj_vector_zero   $'0\n0\n0\n0\n0\n1\n2'
check leak        tests/samples/proj_leak      "1"
check cycle       tests/samples/proj_cycle     "0"
check scalars     tests/samples/proj_scalars   $'0\n0\n5000000000\n4000000000\ntrue\n-1\nfalse\ntrue'
check floats      tests/samples/proj_floats    $'3.75\n3.375\n7\ntrue\n10\n1.5'
check floats_fn   tests/samples/proj_floats_fn $'10'
check promote     tests/samples/proj_promote   $'5\n5\n12\n13\nfalse\ntrue'
check promote_signed tests/samples/proj_promote_signed $'8\n2\n15\n1\n0\n10000000005'
check string      tests/samples/proj_string    $'Hello, Breezy\n13'
check str_concat_val tests/samples/proj_str_concat_value $'Score: 42\n42 points\nx=3 y=7\nok? true\nd=1.5\nab'
check array       tests/samples/proj_array     $'30'
check array_obj   tests/samples/proj_array_obj $'4\n5'
check array_cycle tests/samples/proj_array_cycle $'0'
check_abort array_oob tests/samples/bad_array_oob.bzy
check stringbuilder tests/samples/proj_sb        $'ababab'
check map         tests/samples/proj_map       $'11\n2\nfalse\n2'
check map_contains tests/samples/proj_map_contains $'true\nfalse\ntrue\nfalse'
check map_keys_values tests/samples/proj_map_keys_values $'60\n3'
check map_entries tests/samples/proj_map_entries $'60\n3'
check map_obj     tests/samples/proj_map_obj   $'6\n9'
check map_cycle   tests/samples/proj_map_cycle $'0'
check map_long    tests/samples/proj_map_long    $'1\n2\ntrue\n2'
check map_byte    tests/samples/proj_map_byte    $'7\n8\n2'
check map_uint    tests/samples/proj_map_uint    $'5\n6\n2'
check map_enum_key   tests/samples/proj_map_enum_key    $'100\n300\nfalse\n2'
check map_object_key tests/samples/proj_map_object_key  $'42\nfalse\n1'
check map_object_cycle tests/samples/proj_map_object_cycle  $'0'
check_fail map_float tests/samples/bad_map_float
check incdec      tests/samples/proj_incdec    $'42\n40'
check break_loop  tests/samples/proj_break     $'8'
check_fail break_outside tests/samples/bad_break_outside.bzy
check for_loop    tests/samples/proj_for       $'55\n15'
check foreach_array  tests/samples/proj_foreach_array  $'30'
check foreach_string tests/samples/proj_foreach_string $'198'
check foreach_map    tests/samples/proj_foreach_map    $'6\n3'
check foreach_pair   tests/samples/proj_foreach_pair   $'60\n3'
check_fail foreach_noniter tests/samples/bad_foreach_noniter.bzy
check box_int    tests/samples/proj_box_int    $'7\ntrue\nfalse'
check box_string tests/samples/proj_box_string $'true\nfalse'
check box_obj    tests/samples/proj_box_obj    $'2\n2'
check box_cycle  tests/samples/proj_box_cycle  $'0'
check list_int    tests/samples/proj_list_int     $'3\n99\n2\nfalse\n99'
check stack_queue tests/samples/proj_stack_queue  $'2\n2\n1\n7\n7\n8'
check deque       tests/samples/proj_deque        $'1\n3\n1\n3\n1'
check set_string  tests/samples/proj_set_string   $'2\ntrue\nfalse\nfalse\n1'
check set_obj     tests/samples/proj_set_obj       $'2\ntrue\ntrue\nfalse'
check set_obj_cycle tests/samples/proj_set_obj_cycle $'0'
check set_enum    tests/samples/proj_set_enum      $'2\ntrue\nfalse'
check set_long    tests/samples/proj_set_long      $'2\ntrue\ntrue'
check_fail set_float tests/samples/bad_set_float
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
check clock_date  tests/samples/proj_clock_date    $'1970-01-01 00:00:00\n1970/01/01 00:00'
check random      tests/samples/proj_random        $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\n4'
check regex       tests/samples/proj_regex         $'true\ntrue\n123\na#b#c#'
check str_query   tests/samples/proj_str_query     $'true\ntrue\n5\n10'
check str_transform tests/samples/proj_str_transform $'Hello.World\nHello World\nHELLO WORLD\nHello'
check str_more    tests/samples/proj_str_more      $'true\nfalse\n101\n4\nababab'
check str_split   tests/samples/proj_str_split     $'3\nred\ngreen\nblue'
check str_isnum   tests/samples/proj_str_isnum     $'true\ntrue\nfalse\ntrue\nfalse'
# System.args(): run the program WITH arguments and confirm they arrive (count + values).
bzy_build tests/samples/proj_args
if [ $? -eq 0 ]; then
    got="$(./out.exe alpha beta)"; got="${got//$'\r'/}"
    if [ "$got" == $'2\nalpha\nbeta' ]; then echo "  args: OK"; else echo "  args: FAIL (got '$got')"; fail=1; fi
else
    echo "  args: COMPILE FAILED"; fail=1
fi
check parse       tests/samples/proj_parse         $'42\n7\ntrue'
check_throws throw tests/samples/proj_throw "boom"
check catch       tests/samples/proj_catch         $'caught it\n0'
check catch_multi tests/samples/proj_catch_multi   $'bee\n99'
check catch_subclass tests/samples/proj_catch_subclass $'missing\n0'
check rethrow     tests/samples/proj_rethrow       "bee"
check catch_oob   tests/samples/proj_catch_oob     $'Array index 5 out of bounds for length 3.\n99'
check catch_param tests/samples/proj_catch_param   $'7'
check interface_poly tests/samples/proj_interface_poly $'woof\nmeow'
check_fail generic_bound   tests/samples/bad_generic_bound
check_fail generic_arity   tests/samples/bad_generic_arity
check_fail generic_unknown tests/samples/bad_generic_unknown
check generic_box   tests/samples/proj_generic_box   $'7\nhi'
check generic_pair  tests/samples/proj_generic_pair  $'42\nanswer'
check generic_announce tests/samples/proj_generic_bound $'woof\nmeow'
check static_field tests/samples/proj_static_field $'1\n2\n3\n3'
check static_class tests/samples/proj_static_class $'100\nBreezy\n50\n42'
check static_singleton tests/samples/proj_static_singleton $'hello'
check_fail static_new     tests/samples/bad_static_new.bzy
check_fail static_ctor    tests/samples/bad_static_ctor.bzy
check_fail static_unknown tests/samples/bad_static_unknown.bzy
check_fail field_init_instance tests/samples/bad_field_init_instance.bzy
check enum_basic tests/samples/proj_enum_basic $'1\nGREEN\n255\n255\n3\n255'
check enum_body  tests/samples/proj_enum_body  $'7\n7\n42\nADD\nOp'
check enum_switch tests/samples/proj_enum_switch $'2'
check switch_string tests/samples/proj_switch_string $'2'
check switch_bool   tests/samples/proj_switch_bool   $'20'
check switch_long   tests/samples/proj_switch_long   $'20\n30'
check_fail switch_float tests/samples/bad_switch_float
check enum_iface tests/samples/proj_enum_iface $'HI'
check_fail enum_reserved   tests/samples/bad_enum_reserved_method.bzy
check_fail enum_arity      tests/samples/bad_enum_arity.bzy
check_fail enum_extends    tests/samples/bad_enum_extends.bzy
check_fail enum_bad_label  tests/samples/bad_enum_switch_label.bzy
check record_basic tests/samples/proj_record_basic $'3\n4'
check_fail record_extends tests/samples/bad_record_extends
check record_methods tests/samples/proj_record_methods $'true\nfalse\ntrue'
check record_map_key  tests/samples/proj_record_map_key  $'42\ntrue\nfalse\n1'
check record_set      tests/samples/proj_record_set      $'2\ntrue\nfalse'
check record_nested   tests/samples/proj_record_nested   $'7\nfalse'
check record_identity_fallback tests/samples/proj_record_identity_fallback $'true\nfalse'
check record_cycle    tests/samples/proj_record_cycle    $'0'
check file_exists tests/samples/proj_file_exists   $'true\nfalse\nfalse\ncaught'
check file_rw     tests/samples/proj_file_rw        $'alpha\nbeta\ngamma\n\n3\n14\n4\n3\n42'
check file_search tests/samples/proj_file_search    $'4\n2\n3\nfalse'
check file_attr   tests/samples/proj_file_attr       $'true\nfalse\ntrue\nfalse'
# Cooperative yield ordering only holds on a single worker; pin these two.
export BZY_WORKERS=1
check spawn       tests/samples/proj_spawn         $'1\n2\n9\n3\n4'
check spawn_args  tests/samples/proj_spawn_args    $'7\nhi'
check timer_crash tests/samples/proj_timer_crash   $'42'
check_abort timer_deadlock tests/samples/bad_timer_deadlock
unset BZY_WORKERS
check channel     tests/samples/proj_channel       $'60'
check mc_sum      tests/samples/proj_mc_sum        $'100'
check mc_shared   tests/samples/proj_mc_shared     $'42'
check mc_stress   tests/samples/proj_mc_stress     $'124500'
check file_async  tests/samples/proj_file_async    $'50'
check system_shell  tests/samples/proj_system_shell  $'5'
check classname     tests/samples/proj_classname      $'Dog'
check tcp_echo      tests/samples/proj_tcp_echo       $'4'
check udp_echo      tests/samples/proj_udp_echo       $'2'
check socket_timeout tests/samples/proj_socket_timeout $'2'
check filechannel   tests/samples/proj_filechannel    $'10'
check filewriter    tests/samples/proj_filewriter     $'102'
check logger        tests/samples/proj_logger         $'200'
check ffi           tests/samples/proj_ffi            $'5\n7'
# FFI --link flag: recompile with an extra -l and confirm it still links + runs.
./breezy tests/samples/proj_ffi --link m >/dev/null 2>&1
if [ $? -eq 0 ]; then
    got="$(./out.exe)"; got="${got//$'\r'/}"
    if [ "$got" == $'5\n7' ]; then echo "  ffi_link: OK"; else echo "  ffi_link: FAIL (got '$got')"; fail=1; fi
else
    echo "  ffi_link: COMPILE FAILED"; fail=1
fi
check ffi_toml      tests/samples/proj_ffi_toml       $'5\n7'
check_fail ffi_toml_badlib tests/samples/proj_ffi_badlib
check blocking      tests/samples/proj_blocking       $'5\n7'
check timer_after tests/samples/proj_timer_after   $'1\n2'
check timer_order tests/samples/proj_timer_order   $'1\n2\n3'
check vec2        tests/samples/proj_vec2          $'5\nfalse\ntrue\n5'
check vec3        tests/samples/proj_vec3          $'7\nfalse\ntrue\ntrue\n0'
check_fail narrow_no_cast tests/samples/bad_narrow.bzy
check_fail mixed_sign     tests/samples/bad_mixed_sign.bzy
check_fail default_order  tests/samples/bad_default_order.bzy
check_fail int_condition  tests/samples/bad_int_cond.bzy
check_fail bool_int_cast  tests/samples/bad_bool_cast.bzy
check_fail float_needs_cast tests/samples/bad_int_to_float.bzy
check_fail dbl_to_float     tests/samples/bad_double_to_float.bzy
check_fail schedule_nonvoid tests/samples/bad_schedule_nonvoid
check_fail schedule_args    tests/samples/bad_schedule_args
# Cross-assembly: the Linux (System V / ELF64) emission must assemble cleanly with
# NASM. Link + run is a Linux-only step; here we only prove the asm is valid ELF64.
# Skipped (not failed) if NASM can't emit elf64 on this host.
if nasm -hf 2>/dev/null | grep -qi elf64; then
    ./breezy tests/samples/arith.bzy --target linux >/dev/null 2>&1
    if [ -f out.asm ] && nasm -f elf64 out.asm -o out_elf.o >/dev/null 2>&1; then
        echo "  elf64_assembles: OK"
    else
        echo "  elf64_assembles: FAIL"; fail=1
    fi
    rm -f out_elf.o
else
    echo "  elf64_assembles: SKIP (nasm has no elf64 output on this host)"
fi
if [ $fail -eq 0 ]; then echo "All integration tests passed"; else echo "FAILURES"; exit 1; fi
