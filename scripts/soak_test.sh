#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
duration="${1:-1800}"
log="${2:-soak-$(date +%Y%m%d-%H%M%S).log}"
resource_log="${log}.resources"

export LD_LIBRARY_PATH="/usr/local/runtime/lib/aarch64:/usr/local/lib:${LD_LIBRARY_PATH:-}"
started_epoch="$(date +%s)"
stdbuf -oL -eL ./build/bsaps_app --duration "$duration" >"$log" 2>&1 &
app_pid=$!
trap 'kill -TERM "$app_pid" 2>/dev/null || true' EXIT

for _ in {1..30}; do
  if grep -q '\[camera\] capture started' "$log"; then
    break
  fi
  sleep 1
done
grep -q '\[camera\] capture started' "$log" || { echo "application did not start" >&2; exit 1; }

(
  while kill -0 "$app_pid" 2>/dev/null; do
    timestamp="$(date --iso-8601=seconds)"
    rss_kb="$(awk '/VmRSS/{print $2}' /proc/$app_pid/status)"
    fd_count="$(find /proc/$app_pid/fd -mindepth 1 -maxdepth 1 | wc -l)"
    echo "$timestamp rss_kb=$rss_kb fd_count=$fd_count"
    sleep 30
  done
) >"$resource_log" &
monitor_pid=$!

python3 ./scripts/rtsp_lifecycle_test.py \
  rtsp://127.0.0.1:8554/stream --cycles 30 --settle-ms 0 --timeout 10
./scripts/verify_rtsp.sh rtsp://127.0.0.1:8554/stream 3 10
wait "$app_pid"
wait "$monitor_pid" || true
trap - EXIT
elapsed="$(( $(date +%s) - started_epoch ))"
if (( elapsed + 5 < duration )); then
  echo "soak test ended early: elapsed=${elapsed}s requested=${duration}s" >&2
  exit 1
fi

{
  echo "kernel_errors_begin"
  dmesg --color=never | grep -E 'libcamera|bcm2835-codec|videobuf2' | tail -n 80 || true
  echo "kernel_errors_end"
} >>"$log"

grep -q '\[app\] stopped .*outstanding=0' "$log" || {
  echo "soak test ended without clean lease release" >&2
  exit 1
}
echo "soak test passed: $log (resources: $resource_log)"
