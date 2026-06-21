#!/usr/bin/env bash
# scripts/stress.sh — stress Core across sanitizer + leak passes, collect crash reports,
# then analyze logs (STRESS_TEST.md §7). DO NOT ship in release (dev/CI only).
#
#   ./scripts/stress.sh [ITERS] [SEED]
#
# Each pass builds into its own build tree (build-asan / build-tsan / build-leak) so sanitizer
# flags don't mix. ASan and TSan are mutually exclusive -> run separately. macOS has no reliable
# LeakSanitizer -> use `leaks --atExit`. Artifacts (log + crash report + summary) collected into stress-artifacts/<ts>.
set -euo pipefail
cd "$(dirname "$0")/.."

ITERS="${1:-50000}"
SEED="${2:-42}"

# Invoke cmake via mise if available (ensures correct toolchain + VCPKG_ROOT), like the Makefile.
if command -v mise >/dev/null 2>&1; then RUN="mise exec --"; else RUN=""; fi

TS=$(date +%Y%m%d-%H%M%S); ART="stress-artifacts/$TS"; mkdir -p "$ART"
DR="$HOME/Library/Logs/DiagnosticReports"
BEFORE=$(ls "$DR" 2>/dev/null | sort || true)

# $1=name  $2=extra cmake flags
build_pass () {
  local name="$1"; shift
  echo ">>> configure+build pass: $name ($*)"
  $RUN cmake -S . -B "build-$name" -DCMAKE_BUILD_TYPE=Debug \
       -DBUILD_MACOS_UI=OFF -DBUILD_TESTS=OFF -DDEED_BUILD_STRESS=ON "$@" >/dev/null
  $RUN cmake --build "build-$name" --target deed_stress -j >/dev/null
}

run_bin () { # $1=name
  local name="$1"
  DEED_STRESS=1 MallocStackLogging=1 \
    "./build-$name/ui/cli/deed_stress" --iters "$ITERS" --seed "$SEED" \
    --log "$ART/core-$name.csv" 2>&1 | tee "$ART/core-$name.out" || true
}

# --- ASan pass: use-after-free / overflow ---
build_pass asan -DENABLE_ASAN=ON
ASAN_OPTIONS=detect_leaks=0 run_bin asan

# --- TSan pass: data race (ResponseCache background put vs get, ThreadPool) ---
build_pass tsan -DENABLE_TSAN=ON
run_bin tsan

# --- Leak pass: normal build + leaks --atExit ---
build_pass leak
echo ">>> leaks --atExit"
DEED_STRESS=1 MallocStackLogging=1 \
  leaks --atExit -- "./build-leak/ui/cli/deed_stress" --iters "$ITERS" --seed "$SEED" \
  --log "$ART/core-leak.csv" 2>&1 | tee "$ART/leaks.out" || true

# --- Crash reports newly generated during the run ---
AFTER=$(ls "$DR" 2>/dev/null | sort || true)
comm -13 <(echo "$BEFORE") <(echo "$AFTER") | while read -r f; do
  [ -n "$f" ] && cp "$DR/$f" "$ART/" 2>/dev/null || true
done

# --- Analyze logs (slope/baseline/cap) ---
python3 scripts/analyze.py "$ART"/*.csv | tee "$ART/summary.txt" || true
echo "Artifacts: $ART"
