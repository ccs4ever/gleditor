#!/usr/bin/env python3
# Since Stage 5 of design/priority-page-building.md, "settled" no longer means
# "every page of every document was built" -- a document larger than the
# viewport settles having banked most of its pages, never turning them into
# GPU resources. "Complete render settled" now measures time to satisfy the
# viewport (and any priority offsets), not time to build the whole document,
# so it is expected to land close to "First page rendered" rather than scale
# with document size the way it did pre-Stage-5 -- see
# design/kjv-load-blocking-regression.md for those before figures. "Built
# pages" (new) is the steady-state page count banking leaves on the GPU;
# "Total pages" is the document's length, which banking does not shrink.
import os
import subprocess
import re
import sys
import statistics
import time

SAMPLE = "tests/samples/kjv.txt"
BIN = "build/gleditor"
RUNS = 3
COUNTS = [1, 2, 3]
BACKENDS = ["opengl", "vulkan"]

if not os.path.exists(SAMPLE):
    sys.exit(f"Error: {SAMPLE} not found")

print("=" * 88)
print("       GLEDITOR LOAD & RENDER BENCHMARK: KJV.TXT (4.4 MB per document)")
print("=" * 88)

results = {}
total_steps = len(BACKENDS) * len(COUNTS)
step = 0
start_bench_time = time.time()

for backend in BACKENDS:
    print(f"\n=== Testing Backend: {backend.upper()} ===")
    results[backend] = {}
    for count in COUNTS:
        step += 1
        args = [BIN, "--backend", backend, "--profile"] + [SAMPLE] * count
        ttfp_runs = []
        settle_runs = []
        total_pages_detected = 0
        built_pages_detected = 0
        total_mb = count * 4.4

        print(f"\n[{step}/{total_steps}] {backend.upper()} | {count} doc(s) ({total_mb:.1f} MB, {count * 1352} total pages):")

        for r in range(RUNS):
            print(f"  -> Run {r + 1}/{RUNS} running... ", end="", flush=True)
            run_start = time.time()

            env = dict(os.environ)
            env["SDL_VIDEODRIVER"] = "offscreen"
            res = subprocess.run(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                env=env,
            )
            elapsed = time.time() - run_start
            output = res.stdout

            ttfp_match = re.search(
                r"First page rendered:\s*([0-9.]+)\s*ms", output
            )
            settle_match = re.search(
                r"Complete render settled:\s*([0-9.]+)\s*ms.*?total pages:\s*([0-9]+),"
                r"\s*built pages:\s*([0-9]+)",
                output,
            )

            if ttfp_match and settle_match:
                ttfp = float(ttfp_match.group(1))
                settle = float(settle_match.group(1))
                total_pages = int(settle_match.group(2))
                built_pages = int(settle_match.group(3))
                ttfp_runs.append(ttfp)
                settle_runs.append(settle)
                total_pages_detected = total_pages
                built_pages_detected = built_pages
                print(
                    f"done in {elapsed:.1f}s (TTFP: {ttfp:.1f} ms, Settled: {settle:.1f} ms, "
                    f"Built/Total pages: {built_pages}/{total_pages})"
                )
            else:
                print(
                    f"FAILED (code {res.returncode}, {elapsed:.1f}s)\n{output[:400]}"
                )

        if ttfp_runs and settle_runs:
            avg_ttfp = statistics.mean(ttfp_runs)
            med_ttfp = statistics.median(ttfp_runs)
            avg_settle = statistics.mean(settle_runs)
            med_settle = statistics.median(settle_runs)

            results[backend][count] = {
                "total_pages": total_pages_detected,
                "built_pages": built_pages_detected,
                "ttfp_avg": avg_ttfp,
                "ttfp_med": med_ttfp,
                "settle_avg": avg_settle,
                "settle_med": med_settle,
                "mb": total_mb,
            }
            print(
                f"  => Summary: Avg TTFP {avg_ttfp:.1f} ms | Avg Settled {avg_settle:.1f} ms | "
                f"Built/Total pages {built_pages_detected}/{total_pages_detected}"
            )
        else:
            print("  => FAILED")

total_elapsed = time.time() - start_bench_time

print("\n" + "=" * 88)
print(f"                       BENCHMARK RESULTS (Total Time: {total_elapsed:.1f}s)                       ")
print("=" * 88)

print(
    f"{'Backend':<8} | {'Docs':<4} | {'Total Size':<10} | {'Built/Total Pages':<18} | {'TTFP (Avg)':<12} | {'Settled (Avg)':<14}"
)
print("-" * 88)

for backend in BACKENDS:
    for count in COUNTS:
        if count in results[backend]:
            d = results[backend][count]
            pages = f"{d['built_pages']}/{d['total_pages']}"
            print(
                f"{backend:<8} | {count:<4} | {d['mb']:<8.1f}MB | {pages:<18} | {d['ttfp_avg']:<9.1f} ms | {d['settle_avg']:<11.1f} ms"
            )
    print("-" * 88)
