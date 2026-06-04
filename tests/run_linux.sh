#!/usr/bin/env bash
# Linux (ELF64) verification for the Part 8-2 runtime: the compute + concurrency
# subset that the Linux backend supports today (no files/sockets/HTTP yet). Run
# on a Linux host after `make`. Mirrors the expected outputs in
# run_integration.sh; the compiled binary is out.exe on both platforms.
set -u
sed -i 's/\r$//' "$0" 2>/dev/null   # Tolerate CRLF if the repo was checked out on Windows.
fail=0
check() {
    local name="$1" target="$2" expected="$3"
    ./breezy "$target" >/dev/null 2>&1 || { echo "  $name: COMPILE FAILED"; fail=1; return; }
    local got; got="$(./out.exe 2>&1)"; got="${got//$'\r'/}"
    if [ "$got" == "$expected" ]; then echo "  $name: OK"
    else echo "  $name: FAIL (expected '$expected', got '$got')"; fail=1; fi
}
echo "Linux ELF64 integration (compute + concurrency subset)"
check minimal     tests/samples/minimal.bzy    "0"
check arith       tests/samples/arith.bzy      "14"
check if_else     tests/samples/if_else.bzy    "1"
check while       tests/samples/while.bzy      "10"
check multi_fn    tests/samples/multi_fn.bzy   "42"
check inheritance tests/samples/proj_inherit   "2"
check scalars     tests/samples/proj_scalars   $'0\n0\n5000000000\n4000000000\ntrue\n-1\nfalse\ntrue'
check floats      tests/samples/proj_floats    $'3.75\n3.375\n7\ntrue\n10\n1.5'
check string      tests/samples/proj_string    $'Hello, Breezy\n13'
check array       tests/samples/proj_array     "30"
check map         tests/samples/proj_map       $'11\n2\nfalse\n2'
check list_int    tests/samples/proj_list_int  $'3\n99\n2\nfalse\n99'
check switch      tests/samples/proj_switch    $'20\n30\n10\n99'
check foreach_array tests/samples/proj_foreach_array $'30'
check foreach_map   tests/samples/proj_foreach_map   $'6\n3'
check str_split   tests/samples/proj_str_split  $'3\nred\ngreen\nblue'
check parse       tests/samples/proj_parse      $'42\n7\ntrue'
check str_isnum   tests/samples/proj_str_isnum  $'true\ntrue\nfalse\ntrue\nfalse'
check catch       tests/samples/proj_catch      $'caught it\n0'
check catch_param tests/samples/proj_catch_param $'7'
check interface_poly tests/samples/proj_interface_poly $'woof\nmeow'
check math_basic  tests/samples/proj_math_basic $'4\n5\n2.5'
check vec2        tests/samples/proj_vec2       $'5\nfalse\ntrue\n5'
# Concurrency: cooperative single-worker ordering is deterministic (pin to 1).
export BZY_WORKERS=1
check spawn       tests/samples/proj_spawn      $'1\n2\n9\n3\n4'
check spawn_args  tests/samples/proj_spawn_args $'7\nhi'
# Timer ordering is only deterministic on one worker: with several, two timers
# whose deadlines are close get popped by different workers and print out of order.
check timer_after tests/samples/proj_timer_after $'1\n2'
check timer_order tests/samples/proj_timer_order $'1\n2\n3'
unset BZY_WORKERS
check channel     tests/samples/proj_channel    $'60'
check mc_sum      tests/samples/proj_mc_sum     $'100'
# File I/O (offload pool + POSIX file ops).
check file_exists tests/samples/proj_file_exists $'true\nfalse\nfalse\ncaught'
check file_rw     tests/samples/proj_file_rw     $'alpha\nbeta\ngamma\n\n3\n14\n4\n3\n42'
check file_search tests/samples/proj_file_search $'4\n2\n3\nfalse'
check file_async  tests/samples/proj_file_async  $'50'
# Attributes diverge by host: READONLY maps to the POSIX write bit (chmod), but
# HIDDEN/SYSTEM/ARCHIVE are Windows-only — on POSIX they are no-ops (hidden is the
# leading-dot convention), so the 3rd line is 'false' here vs 'true' on Windows.
check file_attr   tests/samples/proj_file_attr   $'true\nfalse\nfalse\nfalse'
check filewriter  tests/samples/proj_filewriter  $'102'
check logger      tests/samples/proj_logger      $'200'
check filechannel tests/samples/proj_filechannel  $'10'
# TCP sockets (epoll reactor): loopback accept/connect/read/write + timeout.
check tcp_echo       tests/samples/proj_tcp_echo       $'4'
check socket_timeout tests/samples/proj_socket_timeout $'2'
check udp_echo       tests/samples/proj_udp_echo       $'2'
if [ $fail -eq 0 ]; then echo "All Linux integration tests passed"; else echo "FAILURES"; exit 1; fi
