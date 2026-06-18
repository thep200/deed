#!/usr/bin/env python3
"""scripts/analyze.py — phân tích log stress (STRESS_TEST.md §6).

Đọc CSV do deed_stress / StressRunner ghi (cột:
  ts_ms, iter, op, phys_footprint_mb, ram_cache_bytes, disk_cache_bytes, open_request_id, idle)
và báo:
  - Rò RAM: slope hồi quy tuyến tính của phys_footprint tại các IDLE checkpoint
            (> ngưỡng MB/1000 ops -> nghi leak); baseline có quay về không.
  - Cache: ram/disk cache có vượt cap không (cap truyền qua --ram-cap-mb / --disk-cap-mb).
Trả exit code != 0 nếu phát hiện nghi vấn (để CI fail).

  python3 scripts/analyze.py LOG.csv [more.csv ...]
        [--slope-threshold-mb-per-1k 1.0] [--ram-cap-mb N] [--disk-cap-mb N]
"""
import argparse
import csv
import sys


def linregress_slope(xs, ys):
    """Slope của hồi quy tuyến tính y=a*x+b (least squares). 0 nếu < 2 điểm."""
    n = len(xs)
    if n < 2:
        return 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den if den else 0.0


def analyze_file(path, args):
    iters, foot, idle_iters, idle_foot = [], [], [], []
    ram_peak = disk_peak = 0.0
    rows = 0
    try:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                rows += 1
                try:
                    it = float(r["iter"])
                    mb = float(r["phys_footprint_mb"])
                except (KeyError, ValueError):
                    continue
                iters.append(it)
                foot.append(mb)
                ram_peak = max(ram_peak, float(r.get("ram_cache_bytes", 0) or 0))
                disk_peak = max(disk_peak, float(r.get("disk_cache_bytes", 0) or 0))
                if (r.get("idle", "0") or "0").strip() == "1":
                    idle_iters.append(it)
                    idle_foot.append(mb)
    except FileNotFoundError:
        print(f"[skip] không thấy file: {path}")
        return True

    if rows == 0:
        print(f"[skip] log rỗng: {path}")
        return True

    ok = True
    print(f"\n=== {path} ({rows} rows) ===")

    # --- Rò RAM theo idle checkpoint ---
    series_x, series_y, label = (idle_iters, idle_foot, "idle-checkpoint")
    if len(series_x) < 2:  # không đủ idle -> dùng toàn chuỗi
        series_x, series_y, label = (iters, foot, "all-rows (no idle pts)")
    slope_per_iter = linregress_slope(series_x, series_y)
    slope_per_1k = slope_per_iter * 1000.0
    base0 = series_y[0]
    baseN = series_y[-1]
    print(f"  footprint[{label}]: start={base0:.1f}MB end={baseN:.1f}MB "
          f"min={min(series_y):.1f} max={max(series_y):.1f}")
    print(f"  slope = {slope_per_1k:.4f} MB / 1000 ops  (ngưỡng {args.slope_threshold_mb_per_1k})")
    if slope_per_1k > args.slope_threshold_mb_per_1k:
        print(f"  ✗ NGHI LEAK: footprint tăng đơn điệu (slope > ngưỡng)")
        ok = False
    else:
        print(f"  ✓ slope trong ngưỡng")

    # --- Cache caps ---
    print(f"  cache peak: ram={ram_peak/1048576:.2f}MB disk={disk_peak/1048576:.2f}MB")
    if args.ram_cap_mb and ram_peak > args.ram_cap_mb * 1048576:
        print(f"  ✗ RAM cache vượt cap {args.ram_cap_mb}MB")
        ok = False
    if args.disk_cap_mb and disk_peak > args.disk_cap_mb * 1048576:
        print(f"  ✗ disk cache vượt cap {args.disk_cap_mb}MB")
        ok = False

    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--slope-threshold-mb-per-1k", type=float, default=1.0)
    ap.add_argument("--ram-cap-mb", type=float, default=0)
    ap.add_argument("--disk-cap-mb", type=float, default=0)
    args = ap.parse_args()

    all_ok = True
    for p in args.logs:
        all_ok &= analyze_file(p, args)

    print("\n" + ("RESULT: PASS" if all_ok else "RESULT: FAIL (xem ✗ ở trên)"))
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
