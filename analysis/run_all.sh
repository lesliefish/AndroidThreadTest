#!/bin/bash
# run_all.sh — Execute benchmarks on both platforms and collect CSV output
# Usage: ./run_all.sh [prime_upper_bound] [runs_per_config]
#   Default: prime_upper_bound=1000000, runs_per_config=10

set -e

PRIME_BOUND=${1:-1000000}
RUNS=${2:-10}
OUTPUT_DIR="${3:-./results}"

mkdir -p "$OUTPUT_DIR"

echo "============================================"
echo " Thread Benchmark Runner"
echo " Prime bound: $PRIME_BOUND"
echo " Runs per config: $RUNS"
echo " Output dir: $OUTPUT_DIR"
echo "============================================"

# ─── Android ───────────────────────────────────────────────────────
echo ""
echo ">>> Running Android benchmark..."
ANDROID_OUTPUT="$OUTPUT_DIR/android_benchmark.csv"

if [ -f "./android/native_bench" ]; then
    ./android/native_bench android "$PRIME_BOUND" "$RUNS" > "$ANDROID_OUTPUT" 2>&1
    echo "  Android results saved to $ANDROID_OUTPUT"
else
    echo "  WARNING: Android native_bench not found. Build it first with NDK."
    echo "  Placeholder output:"
    echo "experiment,platform,num_threads,median_ms,min_ms,max_ms,avg_ms,p95_ms,p99_ms,throughput_ops,speedup" > "$ANDROID_OUTPUT"
fi

# ─── Windows ───────────────────────────────────────────────────────
echo ""
echo ">>> Running Windows benchmark..."
WINDOWS_OUTPUT="$OUTPUT_DIR/windows_benchmark.csv"

if [ -f "./windows/threadtest" ]; then
    ./windows/threadtest windows "$PRIME_BOUND" "$RUNS" > "$WINDOWS_OUTPUT" 2>&1
    echo "  Windows results saved to $WINDOWS_OUTPUT"
else
    echo "  WARNING: Windows threadtest not found. Build it first with CMake."
    echo "  Placeholder output:"
    echo "experiment,platform,num_threads,median_ms,min_ms,max_ms,avg_ms,p95_ms,p99_ms,throughput_ops,speedup" > "$WINDOWS_OUTPUT"
fi

echo ""
echo "Done. Results in $OUTPUT_DIR/"
ls -la "$OUTPUT_DIR"/*.csv
