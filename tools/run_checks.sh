#!/usr/bin/env bash
# Runs every verification gate: the plain test suite, the sanitizers, the
# trace-format validator, and the benchmark.
#
# The sanitizer builds are made here rather than as CMake targets because
# each needs the whole library rebuilt with different instrumentation, and
# TSan additionally needs the suppression file for snapshot mode's
# deliberate overwrite race (see .tsan-suppressions).
set -uo pipefail

cd "$(dirname "$0")/.."
root=$PWD
build=${BUILD_DIR:-build}
cc=$(grep -m1 '^CMAKE_C_COMPILER:' "$build/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
cc=${cc:-cc}
src=(src/pftrace.c src/pf_ring.c src/pb_writer.c)
flags=(-std=c11 -Wall -Wextra -g -D_GNU_SOURCE -Iinclude -Isrc)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
step() { printf '\n=== %s ===\n' "$1"; }
check() { if [ "$1" -eq 0 ]; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }

step "build (-Wall -Wextra must be silent)"
warnings=$(cmake --build "$build" --clean-first 2>&1 | grep -c 'warning:')
echo "compiler warnings: $warnings"
check "$([ "$warnings" -eq 0 ] && echo 0 || echo 1)" "no compiler warnings"

step "unit + stress + no-allocation suite"
(cd "$build" && ctest --output-on-failure)
check $? "ctest"

step "ring under ASan/UBSan"
"$cc" "${flags[@]}" -O1 -fsanitize=address,undefined \
    tests/test_ring.c src/pf_ring.c -o "$tmp/ring_asan" -lpthread || fail=1
(cd "$tmp" && ./ring_asan >/dev/null)
check $? "ring, ASan/UBSan"

step "library under ASan/UBSan"
"$cc" "${flags[@]}" -O1 -fsanitize=address,undefined \
    tests/test_stress.c "${src[@]}" -o "$tmp/stress_asan" -lpthread || fail=1
(cd "$tmp" && ./stress_asan >/dev/null)
check $? "stress, ASan/UBSan"

step "ring under ThreadSanitizer"
"$cc" "${flags[@]}" -O1 -fsanitize=thread \
    tests/test_ring.c src/pf_ring.c -o "$tmp/ring_tsan" -lpthread || fail=1
out=$(cd "$tmp" && TSAN_OPTIONS="suppressions=$root/.tsan-suppressions" \
    ./ring_tsan 2>&1)
n=$(grep -c 'WARNING: ThreadSanitizer' <<<"$out")
echo "TSan warnings: $n"
check "$([ "$n" -eq 0 ] && echo 0 || echo 1)" "ring, TSan race-free"

step "library under ThreadSanitizer"
"$cc" "${flags[@]}" -O1 -fsanitize=thread \
    tests/test_stress.c "${src[@]}" -o "$tmp/stress_tsan" -lpthread || fail=1
out=$(cd "$tmp" && TSAN_OPTIONS="suppressions=$root/.tsan-suppressions" \
    ./stress_tsan 2>&1)
n=$(grep -c 'WARNING: ThreadSanitizer' <<<"$out")
echo "TSan warnings: $n"
check "$([ "$n" -eq 0 ] && echo 0 || echo 1)" "library, TSan race-free"

step "end-to-end: example output validates"
(cd "$build" && ./example_basic >/dev/null) || fail=1
for f in demo_snapshot demo_continuous; do
    (cd "$build" && ./dump_trace "$f.pftrace" >/dev/null)
    check $? "dump_trace $f.pftrace"
done

step "stress traces validate"
(cd "$build" && ./test_stress >/dev/null) || fail=1
for f in stress_snapshot stress_continuous stress_paced stress_names; do
    (cd "$build" && ./dump_trace "$f.pftrace" >/dev/null)
    check $? "dump_trace $f.pftrace"
done

step "schema check against Google's protobuf runtime"
# Strongest available check: parse our hand-rolled bytes with code generated
# by protoc from the real field numbers, so an encoder bug cannot hide behind
# a matching bug in our own decoder. Prefers the perfetto package; falls back
# to compiling tools/perfetto_min.proto, which covers every field we emit.
if python3 -c 'import perfetto' 2>/dev/null; then
    (cd "$build" && python3 ../tools/verify_trace.py --expect-complete \
        demo_continuous.pftrace)
    check $? "verify_trace.py (continuous, complete)"
    (cd "$build" && python3 ../tools/verify_trace.py demo_snapshot.pftrace)
    check $? "verify_trace.py (snapshot window)"
elif command -v nix-shell >/dev/null 2>&1; then
    nix-shell -p protobuf 'python3.withPackages(p: [p.protobuf])' \
        --run "bash $root/tools/check_schema.sh $root $build" \
        2>&1 | grep -v 'structuredAttrs\|^created.*symlinks'
    check "${PIPESTATUS[0]}" "protoc-generated schema check"
else
    echo "SKIP: no perfetto module and no nix-shell"
    echo "      tools/dump_trace.c covers the same structure with no deps"
fi

step "benchmark"
(cd "$build" && ./bench)

printf '\n'
if [ "$fail" -eq 0 ]; then
    echo "ALL GATES PASSED"
else
    echo "SOME GATES FAILED"
fi
exit "$fail"
