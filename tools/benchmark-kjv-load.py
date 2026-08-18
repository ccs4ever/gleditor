#!/usr/bin/env python3
import os
import subprocess
import re
import sys
import statistics

SAMPLE = "tests/samples/kjv.txt"
BIN = "build/gleditor"
RUNS = 3
COUNTS = [1, 2, 3]
BACKENDS = ["opengl", "vulkan"]

if not os.path.exists(SAMPLE):
    sys.exit(f"Error: {SAMPLE} not found")

print("=" * 80)
print("     GLEDITOR LOAD & RENDER BENCHMARK: KJV.TXT (4.4 MB per document)")
print("=" * 80)

results = {}

for backend in BACKENDS:
    print(f"\n--- Testing Backend: {backend.upper()} ---")
    results[backend] = {}
    for count in COUNTS:
        args = [BIN, "--backend", backend, "--profile"] + [SAMPLE] * count
        ttfp_runs = []
        settle_runs = []
        pages_detected = 0

        print(f"  Measuring {count} document(s) ({count * 4.4:.1f} MB)... ", end="", flush=True)

        for r in range(RUNS):
            env = dict(os.environ)
            env["SDL_VIDEODRIVER"] = "offscreen"
            res = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
            output = res.stdout

            ttfp_match = re.search(r"First page rendered:\s*([0-9.]+)\s*ms", output)
            settle_match = re.search(r"Complete render settled:\s*([0-9.]+)\s*ms.*?total pages:\s*([0-9]+)", output)

            if ttfp_match and settle_match:
                ttfp = float(ttfp_match.group(1))
                settle = float(settle_match.group(1))
                pages = int(settle_match.group(2))
                ttfp_runs.append(ttfp)
                settle_runs.append(settle)
                pages_detected = pages
            else:
                print(f"\n[Warning: Run {r+1} failed to parse output. Code: {res.returncode}]\n{output[:500]}")

        if ttfp_runs and settle_runs:
            avg_ttfp = statistics.mean(ttfp_runs)
            med_ttfp = statistics.median(ttfp_runs)
            avg_settle = statistics.mean(settle_runs)
            med_settle = statistics.median(settle_runs)
            total_mb = count * 4.4
            throughput = total_mb / (avg_settle / 1000.0)

            results[backend][count] = {
                "pages": pages_detected,
                "ttfp_avg": avg_ttfp,
                "ttfp_med": med_ttfp,
                "settle_avg": avg_settle,
                "settle_med": med_settle,
                "throughput": throughput,
                "mb": total_mb
            }
            print(f"Done (TTFP: {avg_ttfp:.1f}ms, Complete: {avg_settle/1000.0:.2f}s, Pages: {pages_detected})")
        else:
            print("FAILED")

print("\n" + "=" * 80)
print("                               BENCHMARK RESULTS                                ")
print("=" * 80)

print(f"{'Backend':<8} | {'Docs':<4} | {'Total Size':<10} | {'Total Pages':<11} | {'TTFP (Avg)':<12} | {'Complete (Avg)':<14} | {'Throughput':<12}")
print("-" * 88)

for backend in BACKENDS:
    for count in COUNTS:
        if count in results[backend]:
            d = results[backend][count]
            print(f"{backend:<8} | {count:<4} | {d['mb']:<8.1f}MB | {d['pages']:<11} | {d['ttfp_avg']:<9.1f} ms | {d['settle_avg']/1000.0:<11.2f} s | {d['throughput']:<8.2f} MB/s")
    print("-" * 88)
