#!/usr/bin/env bash
# Builds and runs the test suite three ways: normal (Release, Apple clang),
# ASan+UBSan+LeakSanitizer and TSan (both with Homebrew LLVM).
#
# Why Homebrew LLVM for the sanitizer builds: on macOS 26.6 the ASan/TSan
# runtimes that ship with Apple clang 17 (Command Line Tools 26.2) hang or
# crash during startup, even for hello-world. Homebrew's LLVM runtimes work and
# also support leak detection on Apple Silicon. Override with SAN_CXX=...
set -euo pipefail
cd "$(dirname "$0")/.."
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
SAN_CXX=${SAN_CXX:-/opt/homebrew/opt/llvm/bin/clang++}

run() {
  local dir=$1; shift
  echo "=== $dir ($*)"
  cmake -S . -B "$dir" "$@" >/dev/null
  cmake --build "$dir" -j "$jobs" >/dev/null
  ctest --test-dir "$dir" -j "$jobs" --output-on-failure 2>&1 | tail -3
}

run build -DCMAKE_BUILD_TYPE=Release
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 \
  run build-asan -DCMAKE_BUILD_TYPE=Debug -DLSM_SANITIZER=address -DCMAKE_CXX_COMPILER="$SAN_CXX"
TSAN_OPTIONS=halt_on_error=1 \
  run build-tsan -DCMAKE_BUILD_TYPE=Debug -DLSM_SANITIZER=thread -DCMAKE_CXX_COMPILER="$SAN_CXX"
