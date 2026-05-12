#!/usr/bin/env bash
# profile.sh — profile server_pool under stress and produce a FlameGraph SVG
set -euo pipefail

FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/tmp/FlameGraph}"
BUILD_DIR="$(dirname "$0")/build"
OUT_DIR="$(dirname "$0")/profiling"
FREQ=99          # perf sampling frequency (Hz)
PORT=8080

# ── helpers ──────────────────────────────────────────────────────────────────
die() { echo "ERROR: $*" >&2; exit 1; }

check_flamegraph() {
    [[ -x "$FLAMEGRAPH_DIR/flamegraph.pl" ]] || \
        die "FlameGraph not found at $FLAMEGRAPH_DIR. Run: git clone --depth=1 https://github.com/brendangregg/FlameGraph $FLAMEGRAPH_DIR"
    [[ -x "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ]] || \
        die "stackcollapse-perf.pl missing from $FLAMEGRAPH_DIR"
}

lower_perf_paranoid() {
    local cur
    cur=$(cat /proc/sys/kernel/perf_event_paranoid)
    if [[ "$cur" -gt 1 ]]; then
        echo "perf_event_paranoid=$cur — lowering to 1 (requires sudo)"
        sudo sysctl -w kernel.perf_event_paranoid=1
        # restore on exit
        trap "sudo sysctl -w kernel.perf_event_paranoid=$cur >/dev/null" EXIT
    fi
}

# ── build ─────────────────────────────────────────────────────────────────────
echo "==> Building server_pool_prof and stress..."
cmake -S "$(dirname "$0")" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -Wno-dev
cmake --build "$BUILD_DIR" --target server_pool_prof stress -j"$(nproc)"

mkdir -p "$OUT_DIR"

# ── perf paranoid ─────────────────────────────────────────────────────────────
check_flamegraph
lower_perf_paranoid

# ── launch server ─────────────────────────────────────────────────────────────
echo "==> Starting server_pool_prof on port $PORT..."
"$BUILD_DIR/server_pool_prof" &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null; sudo sysctl -w kernel.perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid) >/dev/null 2>&1 || true" EXIT

# wait for the socket to be ready
for i in $(seq 1 20); do
    if ss -tlnp | grep -q ":$PORT"; then break; fi
    sleep 0.2
done
echo "    server PID=$SERVER_PID"

# ── start perf recording ───────────────────────────────────────────────────────
PERF_DATA="$OUT_DIR/perf.data"
echo "==> Recording at ${FREQ}Hz (perf record -p $SERVER_PID)..."
perf record -F "$FREQ" -p "$SERVER_PID" -g -o "$PERF_DATA" &
PERF_PID=$!

# ── run stress test ────────────────────────────────────────────────────────────
echo "==> Running stress test..."
"$BUILD_DIR/stress" 127.0.0.1 "$PORT" 2>&1 | tee "$OUT_DIR/stress.log"

# ── stop perf ─────────────────────────────────────────────────────────────────
kill -INT "$PERF_PID" 2>/dev/null || true
wait "$PERF_PID" 2>/dev/null || true

kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
trap - EXIT

# ── generate FlameGraph ────────────────────────────────────────────────────────
echo "==> Generating FlameGraph..."
SCRIPT="$OUT_DIR/perf.script"
FOLDED="$OUT_DIR/perf.folded"
SVG="$OUT_DIR/flamegraph.svg"

perf script -i "$PERF_DATA" > "$SCRIPT"

"$FLAMEGRAPH_DIR/stackcollapse-perf.pl" "$SCRIPT" > "$FOLDED"

"$FLAMEGRAPH_DIR/flamegraph.pl" \
    --title "server_pool stress (perf ${FREQ}Hz)" \
    --width 1400 \
    --colors hot \
    "$FOLDED" > "$SVG"

echo ""
echo "==> Done! Open the FlameGraph in your browser:"
echo "    $SVG"
echo ""
echo "    Stress results:"
cat "$OUT_DIR/stress.log"
