#!/usr/bin/env bash
# Builds and runs the test suite three ways: normal (Release), ASan+UBSan, TSan.
set -euo pipefail
cd "$(dirname "$0")/.."
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc)

run() {
  local dir=$1; shift
  echo "=== $dir ($*)"
  cmake -S . -B "$dir" "$@" >/dev/null
  cmake --build "$dir" -j "$jobs" >/dev/null
  ctest --test-dir "$dir" -j "$jobs" --output-on-failure 2>&1 | tail -4
}

run build -DCMAKE_BUILD_TYPE=Release
# ASan on Apple Silicon does not support leak detection; leave it off explicitly.
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
  run build-asan -DCMAKE_BUILD_TYPE=Debug -DLSM_SANITIZER=address
TSAN_OPTIONS=halt_on_error=1 \
  run build-tsan -DCMAKE_BUILD_TYPE=Debug -DLSM_SANITIZER=thread
