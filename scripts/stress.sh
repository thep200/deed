#!/usr/bin/env bash
# scripts/stress.sh — chạy stress Core qua các pass sanitizer + leak, thu crash report,
# rồi phân tích log (STRESS_TEST.md §7). KHÔNG ship release (chỉ dev/CI).
#
#   ./scripts/stress.sh [ITERS] [SEED]
#
# Mỗi pass build ra cây build riêng (build-asan / build-tsan / build-leak) để cờ sanitizer
# không lẫn nhau. ASan và TSan loại trừ nhau -> chạy tách. macOS không có LeakSanitizer tin
# cậy -> dùng `leaks --atExit`. Artifact (log + crash report + summary) gom vào stress-artifacts/<ts>.
set -euo pipefail
cd "$(dirname "$0")/.."

ITERS="${1:-50000}"
SEED="${2:-42}"

# Gọi cmake qua mise nếu có (đảm bảo đúng toolchain + VCPKG_ROOT), như Makefile.
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

# --- TSan pass: data race (ResponseCache put nền vs get, ThreadPool) ---
build_pass tsan -DENABLE_TSAN=ON
run_bin tsan

# --- Leak pass: build thường + leaks --atExit ---
build_pass leak
echo ">>> leaks --atExit"
DEED_STRESS=1 MallocStackLogging=1 \
  leaks --atExit -- "./build-leak/ui/cli/deed_stress" --iters "$ITERS" --seed "$SEED" \
  --log "$ART/core-leak.csv" 2>&1 | tee "$ART/leaks.out" || true

# --- Crash report mới sinh trong khi chạy ---
AFTER=$(ls "$DR" 2>/dev/null | sort || true)
comm -13 <(echo "$BEFORE") <(echo "$AFTER") | while read -r f; do
  [ -n "$f" ] && cp "$DR/$f" "$ART/" 2>/dev/null || true
done

# --- Phân tích log (slope/baseline/cap) ---
python3 scripts/analyze.py "$ART"/*.csv | tee "$ART/summary.txt" || true
echo "Artifacts: $ART"
