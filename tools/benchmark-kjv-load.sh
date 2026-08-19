#!/usr/bin/env bash
set -euo pipefail

SAMPLE="tests/samples/kjv.txt"
BIN="build/gleditor"
RUNS=3

if [ ! -f "$SAMPLE" ]; then
  echo "Error: $SAMPLE not found!"
  exit 1
fi

echo "================================================================================"
echo "          GLEDITOR LOAD TIMING BENCHMARK: KJV.TXT (4.4 MB per document)        "
echo "================================================================================"

for backend in opengl vulkan; do
  echo "=== Backend: ${backend^^} ==="
  for count in 1 2 3; do
    echo "  Measuring $count document(s) ($(python3 -c "print(f'{4.4 * $count:.1f}')") MB)..."
    args=()
    for ((i = 0; i < count; i++)); do
      args+=("$SAMPLE")
    done

    ttfp_list=()
    complete_list=()
    pages=0

    for ((r = 1; r <= RUNS; r++)); do
      echo -n "    -> Run $r/$RUNS in progress... "
      output=$(SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" "$BIN" --backend "$backend" --profile "${args[@]}" 2>&1)

      ttfp=$(echo "$output" | grep -o "First page rendered: [0-9.]*" | head -n1 | awk '{print $4}')
      complete=$(echo "$output" | grep -o "Complete render settled: [0-9.]*" | head -n1 | awk '{print $4}')
      pages=$(echo "$output" | grep -o "total pages: [0-9]*" | head -n1 | awk '{print $3}')

      if [ -n "$ttfp" ] && [ -n "$complete" ]; then
        ttfp_list+=("$ttfp")
        complete_list+=("$complete")
        echo "TTFP: ${ttfp} ms, Complete: $(python3 -c "print(f'{float($complete)/1000.0:.2f}')") s (${pages:-unknown} pages)"
      else
        echo "FAILED"
      fi
    done

    # Compute average
    ttfp_avg=$(python3 -c "import sys; vals=[float(x) for x in sys.argv[1:]]; print(f'{sum(vals)/len(vals):.1f}')" "${ttfp_list[@]}")
    complete_avg_ms=$(python3 -c "import sys; vals=[float(x) for x in sys.argv[1:]]; print(f'{sum(vals)/len(vals):.1f}')" "${complete_list[@]}")
    complete_avg_s=$(python3 -c "import sys; val=float(sys.argv[1]); print(f'{val/1000.0:.2f}s')" "$complete_avg_ms")
    mb_total=$(python3 -c "print(f'{4.4 * $count:.1f} MB')")
    mb_per_sec=$(python3 -c "import sys; sec=float(sys.argv[1])/1000.0; mb=4.4 * $count; print(f'{mb/sec:.2f} MB/s')" "$complete_avg_ms")

    echo "  => Average ($mb_total): TTFP ${ttfp_avg} ms | Complete $complete_avg_s | Throughput $mb_per_sec"
    echo ""
  done
done
