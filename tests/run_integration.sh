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
check_in() {
    # Like check, but feeds $3 to the program's stdin (for input()).
    local name="$1" target="$2" stdin="$3" expected="$4"
    bzy_build "$target"
    if [ $? -ne 0 ]; then echo "  $name: COMPILE FAILED"; fail=1; return; fi
    local got; got="$(printf '%s' "$stdin" | ./out.exe)"
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
check minimal     tests/samples/pass/basics/minimal.bzy    "0"
check arith       tests/samples/pass/basics/arith.bzy      "14"
check int_semantics tests/samples/pass/basics/int_semantics.bzy $'-294967296\n1410065408\n-4\n-3\n-1\n-123\n2\n4000000000\n48\n255\n240\n-2147483648\n1\n16'
check int_defer   tests/samples/pass/basics/int_defer.bzy $'-1000000000\n-2000000000\n1294967296\n294967296\n-705032704'
check region_liveout_shift tests/samples/pass/basics/region_liveout_shift.bzy $'12773843\n303\n707\n1111\n1515\n1919\n2323\n2727'
check unroll_fold tests/samples/pass/basics/unroll_fold.bzy "216"
check int_div     tests/samples/pass/basics/int_div.bzy $'-33\n-1\n33\n33\n-1\n306783378'
check int_dirty   tests/samples/pass/basics/int_dirty.bzy $'1410065408\n88129088\n-851744153\n1\n-1894967296\n1832519941\n15'
check if_else     tests/samples/pass/control-flow/if_else.bzy    "1"
check while       tests/samples/pass/control-flow/while.bzy      "10"
check multi_fn    tests/samples/pass/basics/multi_fn.bzy   "42"
check inheritance tests/samples/pass/oop/proj_inherit  "2"
check inherit_order tests/samples/pass/oop/proj_inherit_order  "2"
check ctor        tests/samples/pass/oop/proj_ctor      $'3
4
7'
check bare_field     tests/samples/pass/oop/bare_field.bzy     $'2\n1\n3'
check default_args   tests/samples/pass/basics/proj_default_args  $'15\n6\n1\n2\n7\n2'
check args_clone_params tests/samples/pass/args/clone_params.bzy  $'7\n9'
check args_five_int     tests/samples/pass/args/five_int.bzy     "15"
check args_eight_method tests/samples/pass/args/eight_method.bzy "28"
check args_mixed_fp     tests/samples/pass/args/mixed_fp.bzy     "14"
check overload_ctor      tests/samples/pass/overload/ctor.bzy      $'7\n14'
check_fail overload_ctor_dup tests/samples/fail/overload/ctor_dup.bzy
check overload_method    tests/samples/pass/overload/method.bzy    $'5\n9\n3.5'
check overload_override  tests/samples/pass/overload/override.bzy   $'50\n5'
check overload_freefn    tests/samples/pass/overload/freefn.bzy     $'42\n5'
check_fail overload_extern_dup tests/samples/fail/overload/extern_dup.bzy
check_fail overload_main_dup   tests/samples/fail/overload/main_dup.bzy
check overload_widening   tests/samples/pass/overload/widening.bzy    $'int\nlong'
check overload_exactclass tests/samples/pass/overload/exact_class.bzy $'10\n20'
check_fail overload_ambig_null      tests/samples/fail/overload/ambiguous_null.bzy
check_fail overload_default_overlap tests/samples/fail/overload/default_overlap.bzy
check datetime_read   tests/samples/pass/datetime/read.bzy          $'2025\n6\n15\n15\n6\n40\n7\n1750000000000'
check datetime_before tests/samples/pass/datetime/epoch_before.bzy  $'1960\n3\n1\n6\n1960\n3'
check datetime_roundtrip tests/samples/pass/datetime/roundtrip.bzy  $'2024\n2\n29\n23\n59\n59\n31\n7'
check datetime_mutate tests/samples/pass/datetime/mutate.bzy        $'2027\n1\n2\n1\n3\n0\n2\n28\n2025\n2\n28\n2\n2'
check datetime_cmpfmt tests/samples/pass/datetime/compare_format.bzy $'true\ntrue\ntrue\nfalse\n2026-06-12 14:30:09\n2026/06/12 14:30'
check_in io_input tests/samples/pass/io/input.bzy $'Breezy\n21\nlast line\n' $'Hi Breezy\n42\n[last line]'
check many_nodes tests/samples/pass/capacity/many_nodes.bzy 60000
check many_instances tests/samples/pass/capacity/many_instances.bzy 300
check six_type_params tests/samples/pass/capacity/six_type_params.bzy 7
check wide_interface tests/samples/pass/capacity/wide_interface.bzy $'17\n55'
check many_interfaces tests/samples/pass/capacity/many_interfaces.bzy $'7\n9'
check big_enum tests/samples/pass/capacity/big_enum.bzy $'79\nC79\n80'
check many_strings tests/samples/pass/capacity/many_strings.bzy 1800
check vector_zero    tests/samples/pass/oop/vector_zero.bzy   $'0\n0\n0\n0\n0\n1\n2'
check desktop_smoke  tests/samples/pass/desktop/smoke.bzy      "desktop-ok"
check_fail desktop_bad_listener tests/samples/fail/desktop/bad_listener_arg.bzy
check leak        tests/samples/pass/memory/proj_leak      "1"
check cycle       tests/samples/pass/memory/proj_cycle     "0"
check scalars     tests/samples/pass/numbers/scalars.bzy   $'0\n0\n5000000000\n4000000000\ntrue\n-1\nfalse\ntrue'
check floats      tests/samples/pass/numbers/floats.bzy    $'3.75\n3.375\n7\ntrue\n10\n1.5'
check floats_fn   tests/samples/pass/numbers/floats_fn.bzy $'10'
check promote     tests/samples/pass/numbers/promote.bzy   $'5\n5\n12\n13\nfalse\ntrue'
check promote_signed tests/samples/pass/numbers/promote_signed.bzy $'8\n2\n15\n1\n0\n10000000005'
check string      tests/samples/pass/strings/string.bzy    $'Hello, Breezy\n13'
check str_concat_val tests/samples/pass/strings/str_concat_value.bzy $'Score: 42\n42 points\nx=3 y=7\nok? true\nd=1.5\nab'
check concat_chain    tests/samples/pass/strings/concat_chain.bzy  $'abcdeab\nn=123'
check str_accum    tests/samples/pass/strings/str_accum.bzy  $'ab0ab1ab2ab3ab4\nx---\nz'
check str_accum_guard tests/samples/pass/strings/str_accum_guard.bzy  $'xxx\n1\n2\n3\nyyy\nzz'
check array       tests/samples/pass/collections/array.bzy     $'30'
check arr_widths   tests/samples/pass/collections/arr_widths.bzy   $'1000000 -7 2000000000 42\n30000 -30000 5\n2001000035\n1'
check box_short    tests/samples/pass/collections/box_short.bzy    $'-12345\n777\ntrue\nfalse'
check arr_bytes    tests/samples/pass/collections/arr_bytes.bzy    $'127 -128 1 -1 0\n-1'
check array_obj   tests/samples/pass/collections/proj_array_obj $'4\n5'
check array_cycle tests/samples/pass/collections/proj_array_cycle $'0'
check_abort array_oob tests/samples/fail/collections/array_oob.bzy
check stringbuilder tests/samples/pass/strings/sb.bzy        $'ababab'
check map         tests/samples/pass/collections/map.bzy       $'11\n2\nfalse\n2'
check map_strkey_cache tests/samples/pass/collections/map_strkey_cache.bzy $'7\ntrue\n7\n12\n1'
check map_contains tests/samples/pass/collections/map_contains.bzy $'true\nfalse\ntrue\nfalse'
check map_keys_values tests/samples/pass/collections/map_keys_values.bzy $'60\n3'
check map_entries tests/samples/pass/collections/map_entries.bzy $'60\n3'
check map_obj     tests/samples/pass/collections/proj_map_obj   $'6\n9'
check map_cycle   tests/samples/pass/collections/proj_map_cycle $'0'
check map_long    tests/samples/pass/collections/map_long.bzy    $'1\n2\ntrue\n2'
check map_byte    tests/samples/pass/collections/map_byte.bzy    $'7\n8\n2'
check map_uint    tests/samples/pass/collections/map_uint.bzy    $'5\n6\n2'
check map_enum_key   tests/samples/pass/collections/proj_map_enum_key    $'100\n300\nfalse\n2'
check map_object_key tests/samples/pass/collections/proj_map_object_key  $'42\nfalse\n1'
check map_object_cycle tests/samples/pass/collections/proj_map_object_cycle  $'0'
check_fail map_float tests/samples/fail/collections/map_float.bzy
check_fail pia_type tests/samples/fail/collections/put_if_absent_type.bzy
check incdec      tests/samples/pass/basics/incdec.bzy    $'42\n40'
check break_loop  tests/samples/pass/control-flow/break.bzy     $'8'
check_fail break_outside tests/samples/fail/control-flow/break_outside.bzy
check for_loop    tests/samples/pass/control-flow/for.bzy       $'55\n15'
check foreach_array  tests/samples/pass/collections/foreach_array.bzy  $'30'
check foreach_string tests/samples/pass/collections/foreach_string.bzy $'198'
check foreach_map    tests/samples/pass/collections/foreach_map.bzy    $'6\n3'
check foreach_pair   tests/samples/pass/collections/foreach_pair.bzy   $'60\n3'
check_fail foreach_noniter tests/samples/fail/collections/foreach_noniter.bzy
check box_int    tests/samples/pass/collections/box_int.bzy    $'7\ntrue\nfalse'
check box_string tests/samples/pass/collections/box_string.bzy $'true\nfalse'
check box_obj    tests/samples/pass/collections/proj_box_obj    $'2\n2'
check box_cycle  tests/samples/pass/collections/proj_box_cycle  $'0'
check list_int    tests/samples/pass/collections/list_int.bzy     $'3\n99\n2\nfalse\n99'
check list_get_front tests/samples/pass/collections/list_get_front.bzy $'1\n10\n20'
check_abort list_get_oob tests/samples/fail/collections/list_get_oob.bzy
check list_set_front tests/samples/pass/collections/list_set_front.bzy $'100\n5\n400'
check_abort list_set_oob tests/samples/fail/collections/list_set_oob.bzy
check stack_queue tests/samples/pass/collections/stack_queue.bzy  $'2\n2\n1\n7\n7\n8'
check deque       tests/samples/pass/collections/deque.bzy        $'1\n3\n1\n3\n1'
check set_string  tests/samples/pass/collections/set_string.bzy   $'2\ntrue\nfalse\nfalse\n1'
check set_obj     tests/samples/pass/collections/proj_set_obj       $'2\ntrue\ntrue\nfalse'
check set_obj_cycle tests/samples/pass/collections/proj_set_obj_cycle $'0'
check set_enum    tests/samples/pass/collections/proj_set_enum      $'2\ntrue\nfalse'
check set_long    tests/samples/pass/collections/set_long.bzy      $'2\ntrue\ntrue'
check_fail set_float tests/samples/fail/collections/set_float.bzy
check foreach_list tests/samples/pass/collections/foreach_list.bzy $'12'
check foreach_list_front tests/samples/pass/collections/foreach_list_front.bzy $'1\n36'
check foreach_set  tests/samples/pass/collections/foreach_set.bzy  $'30\n2'
check list_obj    tests/samples/pass/collections/proj_list_obj      $'6\n8\n7'
check list_cycle  tests/samples/pass/collections/proj_list_cycle    $'0'
check switch      tests/samples/pass/control-flow/switch.bzy        $'20\n30\n10\n99'
check switch_dense  tests/samples/pass/control-flow/switch_dense.bzy  $'100\n101\n102\n103\n104\n999'
check switch_braces tests/samples/pass/control-flow/switch_braces.bzy $'2'
check block_comment tests/samples/pass/basics/block_comment.bzy $'7'
check compound    tests/samples/pass/basics/compound.bzy      $'6\n10\nabc'
check decl_init   tests/samples/pass/basics/decl_init.bzy     $'7\nhi\nRex\nWoof'
check bitwise     tests/samples/pass/operators/bitwise.bzy        $'255\n15\n4\n-1\n10\n16'
check_fail bitwise_float tests/samples/fail/operators/bitwise_float.bzy
check equals_words tests/samples/pass/operators/equals_words.bzy $'true\nfalse\nfalse\ntrue'
check logical     tests/samples/pass/operators/logical.bzy        $'false\ntrue\nfalse\ntrue\ntrue\n7'
check_fail logical_int tests/samples/fail/operators/logical_int.bzy
check alt_spellings tests/samples/pass/operators/alt_spellings.bzy $'false\ntrue\ntrue\ntrue\ntrue\ntrue'
check math_basic  tests/samples/pass/numbers/math_basic.bzy    $'4\n5\n2.5'
check math_minmax tests/samples/pass/numbers/math_minmax.bzy   $'7\n3\n5\n0\n2.5'
check math_round  tests/samples/pass/numbers/math_round.bzy    $'2\n3\n2\n3\ntrue\nfalse'
check math_libm   tests/samples/pass/numbers/math_libm.bzy     $'1024\n1\n1\n0'
check clock       tests/samples/pass/io/clock.bzy         $'true\ntrue'
check clock_date  tests/samples/pass/io/clock_date.bzy    $'1970-01-01 00:00:00\n1970/01/01 00:00'
check random      tests/samples/pass/numbers/random.bzy        $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\n4'
check regex       tests/samples/pass/strings/regex.bzy         $'true\ntrue\n123\na#b#c#'
check str_query   tests/samples/pass/strings/str_query.bzy     $'true\ntrue\n5\n10'
check str_transform tests/samples/pass/strings/str_transform.bzy $'Hello.World\nHello World\nHELLO WORLD\nHello'
check str_more    tests/samples/pass/strings/str_more.bzy      $'true\nfalse\n101\n4\nababab'
check str_split   tests/samples/pass/strings/str_split.bzy     $'3\nred\ngreen\nblue'
check str_isnum   tests/samples/pass/strings/str_isnum.bzy     $'true\ntrue\nfalse\ntrue\nfalse'
# System.args(): run the program WITH arguments and confirm they arrive (count + values).
bzy_build tests/samples/pass/io/args.bzy
if [ $? -eq 0 ]; then
    got="$(./out.exe alpha beta)"; got="${got//$'\r'/}"
    if [ "$got" == $'2\nalpha\nbeta' ]; then echo "  args: OK"; else echo "  args: FAIL (got '$got')"; fail=1; fi
else
    echo "  args: COMPILE FAILED"; fail=1
fi
check parse       tests/samples/pass/numbers/parse.bzy         $'42\n7\ntrue'
check_throws throw tests/samples/pass/exceptions/throw.bzy "boom"
check catch       tests/samples/pass/exceptions/catch.bzy         $'caught it\n0'
check catch_multi tests/samples/pass/exceptions/proj_catch_multi   $'bee\n99'
check catch_subclass tests/samples/pass/exceptions/proj_catch_subclass $'missing\n0'
check rethrow     tests/samples/pass/exceptions/proj_rethrow       "bee"
check catch_oob   tests/samples/pass/exceptions/catch_oob.bzy     $'Array index 5 out of bounds for length 3.\n99'
check catch_param tests/samples/pass/exceptions/catch_param.bzy   $'7'
check interface_poly tests/samples/pass/oop/proj_interface_poly $'woof\nmeow'
check_fail generic_bound   tests/samples/fail/generics/proj_generic_bound
check_fail generic_arity   tests/samples/fail/generics/proj_generic_arity
check_fail generic_unknown tests/samples/fail/generics/generic_unknown.bzy
check generic_box   tests/samples/pass/generics/proj_generic_box   $'7\nhi'
check generic_pair  tests/samples/pass/generics/proj_generic_pair  $'42\nanswer'
check generic_announce tests/samples/pass/generics/proj_generic_bound $'woof\nmeow'
check multi_class_generic tests/samples/pass/generics/multi_class_generic.bzy $'21\n20'
check static_field tests/samples/pass/oop/proj_static_field $'1\n2\n3\n3'
check static_class tests/samples/pass/oop/proj_static_class $'100\nBreezy\n50\n42'
check static_singleton tests/samples/pass/oop/proj_static_singleton $'hello'
check_fail static_new     tests/samples/fail/oop/static_new.bzy
check_fail static_ctor    tests/samples/fail/oop/static_ctor.bzy
check_fail static_unknown tests/samples/fail/oop/static_unknown.bzy
check_fail dup_class_name tests/samples/fail/oop/dup_class_name.bzy
check enum_basic tests/samples/pass/enums/proj_enum_basic $'1\nGREEN\n255\n255\n3\n255'
check enum_body  tests/samples/pass/enums/proj_enum_body  $'7\n7\n42\nADD\nOp'
check enum_switch tests/samples/pass/enums/proj_enum_switch $'2'
check switch_string tests/samples/pass/control-flow/switch_string.bzy $'2'
check switch_bool   tests/samples/pass/control-flow/switch_bool.bzy   $'20'
check switch_long   tests/samples/pass/control-flow/switch_long.bzy   $'20\n30'
check_fail switch_float tests/samples/fail/control-flow/switch_float.bzy
check enum_iface tests/samples/pass/enums/proj_enum_iface $'HI'
check_fail enum_reserved   tests/samples/fail/enums/enum_reserved_method.bzy
check_fail enum_arity      tests/samples/fail/enums/enum_arity.bzy
check_fail enum_extends    tests/samples/fail/enums/enum_extends.bzy
check_fail enum_bad_label  tests/samples/fail/enums/enum_switch_label.bzy
check record_basic tests/samples/pass/records/record_basic.bzy $'3\n4'
check_fail record_extends tests/samples/fail/records/proj_record_extends
check record_methods tests/samples/pass/records/record_methods.bzy $'true\nfalse\ntrue'
check record_map_key  tests/samples/pass/records/proj_record_map_key  $'42\ntrue\nfalse\n1'
check record_set      tests/samples/pass/records/proj_record_set      $'2\ntrue\nfalse'
check record_nested   tests/samples/pass/records/proj_record_nested   $'7\nfalse'
check record_identity_fallback tests/samples/pass/records/proj_record_identity_fallback $'true\nfalse'
check record_cycle    tests/samples/pass/records/proj_record_cycle    $'0'
check file_exists tests/samples/pass/io/file_exists.bzy   $'true\nfalse\nfalse\ncaught'
check file_rw     tests/samples/pass/io/file_rw.bzy        $'alpha\nbeta\ngamma\n\n3\n14\n4\n3\n42'
check file_search tests/samples/pass/io/file_search.bzy    $'4\n2\n3\nfalse'
check file_attr   tests/samples/pass/io/file_attr.bzy       $'true\nfalse\ntrue\nfalse'
# Cooperative yield ordering only holds on a single worker; pin these two.
export BZY_WORKERS=1
check spawn       tests/samples/pass/concurrency/spawn.bzy         $'1\n2\n9\n3\n4'
check spawn_args  tests/samples/pass/concurrency/spawn_args.bzy    $'7\nhi'
check timer_crash tests/samples/pass/concurrency/timer_crash.bzy   $'42'
check_abort timer_deadlock tests/samples/fail/concurrency/timer_deadlock.bzy
unset BZY_WORKERS
check channel     tests/samples/pass/concurrency/channel.bzy       $'60'
check mc_sum      tests/samples/pass/concurrency/mc_sum.bzy        $'100'
check mc_shared   tests/samples/pass/concurrency/mc_shared.bzy     $'42'
check mc_stress   tests/samples/pass/concurrency/mc_stress.bzy     $'124500'
check mc_string   tests/samples/pass/concurrency/mc_string.bzy     $'15000'
check shared_list_hammer tests/samples/pass/concurrency/shared_list_hammer.bzy $'8000'
check shared_map_hammer  tests/samples/pass/concurrency/shared_map_hammer.bzy  $'8000\n500'
check shared_array_obj   tests/samples/pass/concurrency/shared_array_obj.bzy   $'OK'
check pia_race tests/samples/pass/concurrency/put_if_absent_race.bzy $'1\n1\n1'
check mc_pingpong tests/samples/pass/concurrency/mc_pingpong.bzy $'40000'
check file_async  tests/samples/pass/io/file_async.bzy    $'50'
check system_shell  tests/samples/pass/io/system_shell.bzy  $'5'
check classname     tests/samples/pass/oop/proj_classname      $'Dog'
check multi_class    tests/samples/pass/oop/multi_class.bzy   $'15\n7\n7'
check field_init     tests/samples/pass/oop/field_init.bzy  $'4\nrex\ndog\nanimal\n10\n11\n4\n7\n7\n0\n9\n4\nanimal\ndog'
check tcp_echo      tests/samples/pass/net/tcp_echo.bzy       $'4'
check udp_echo      tests/samples/pass/net/udp_echo.bzy       $'2'
check socket_timeout tests/samples/pass/net/socket_timeout.bzy $'2'
check close_wakes_peer tests/samples/pass/net/close_wakes_peer.bzy "ok"
check filechannel   tests/samples/pass/io/filechannel.bzy    $'10'
check readinto      tests/samples/pass/io/readinto.bzy       $'4 65 68\n4 71 74'
check filewriter    tests/samples/pass/io/filewriter.bzy     $'102'
check logger        tests/samples/pass/io/logger.bzy         $'200'
check ffi           tests/samples/pass/ffi/ffi.bzy            $'5\n7'
check ffi_buffer    tests/samples/pass/ffi/ffi_buffer.bzy     $'world\n65\nwor'
check bytes_string  tests/samples/pass/ffi/bytes_string.bzy   $'5\n5\n1\n0'
check callback_qsort tests/samples/pass/ffi/callback_qsort.bzy "1"
# FFI --link flag: recompile with an extra -l and confirm it still links + runs.
./breezy tests/samples/pass/ffi/ffi.bzy --link m >/dev/null 2>&1
if [ $? -eq 0 ]; then
    got="$(./out.exe)"; got="${got//$'\r'/}"
    if [ "$got" == $'5\n7' ]; then echo "  ffi_link: OK"; else echo "  ffi_link: FAIL (got '$got')"; fail=1; fi
else
    echo "  ffi_link: COMPILE FAILED"; fail=1
fi
check ffi_toml      tests/samples/pass/ffi/proj_ffi_toml       $'5\n7'
check_fail ffi_toml_badlib tests/samples/fail/ffi/proj_ffi_badlib
check_fail ffi_obj_array tests/samples/fail/ffi/extern_obj_array.bzy
check_fail frombytes_wrong_array tests/samples/fail/ffi/frombytes_wrong_array.bzy
check blocking      tests/samples/pass/concurrency/blocking.bzy       $'5\n7'
check timer_after tests/samples/pass/concurrency/timer_after.bzy   $'1\n2'
check timer_order tests/samples/pass/concurrency/timer_order.bzy   $'1\n2\n3'
check vec2        tests/samples/pass/oop/vec2.bzy          $'5\nfalse\ntrue\n5'
check vec3        tests/samples/pass/oop/vec3.bzy          $'7\nfalse\ntrue\ntrue\n0'
check_fail narrow_no_cast tests/samples/fail/numbers/narrow.bzy
check_fail mixed_sign     tests/samples/fail/numbers/mixed_sign.bzy
check_fail default_order  tests/samples/fail/basics/default_order.bzy
check_fail int_condition  tests/samples/fail/numbers/int_cond.bzy
check_fail bool_int_cast  tests/samples/fail/numbers/bool_cast.bzy
check_fail float_needs_cast tests/samples/fail/numbers/int_to_float.bzy
check_fail dbl_to_float     tests/samples/fail/numbers/double_to_float.bzy
check_fail schedule_nonvoid tests/samples/fail/concurrency/schedule_nonvoid.bzy
check_fail schedule_args    tests/samples/fail/concurrency/schedule_args.bzy
# Cross-assembly: the Linux (System V / ELF64) emission must assemble cleanly with
# NASM. Link + run is a Linux-only step; here we only prove the asm is valid ELF64.
# Skipped (not failed) if NASM can't emit elf64 on this host.
if nasm -hf 2>/dev/null | grep -qi elf64; then
    ./breezy tests/samples/pass/basics/arith.bzy --target linux >/dev/null 2>&1
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
