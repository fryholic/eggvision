#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
duration="${1:-1800}"
log="${2:-soak-$(date +%Y%m%d-%H%M%S).log}"
events_dir="${3:-/tmp/eggvision-soak-events-$(date +%Y%m%d-%H%M%S)}"
resource_log="${log}.resources"
app="${EGGVISION_APP:-./build/eggvision_app}"
backend="${EGGVISION_BACKEND:-mnn}"
case "$backend" in
  mnn)
    model="${EGGVISION_MODEL:-models/yolov5n.mnn}"
    threads="${EGGVISION_THREADS:-3}"
    ;;
  openvino)
    model="${EGGVISION_MODEL:-models/yolov5n.xml}"
    threads="${EGGVISION_THREADS:-2}"
    ;;
  *)
    echo "EGGVISION_BACKEND must be mnn or openvino" >&2
    exit 2
    ;;
esac

if [[ -e "$events_dir" ]]; then
  echo "soak event path already exists: $events_dir" >&2
  exit 1
fi
if [[ ! -x "$app" ]]; then
  echo "application is not executable: $app" >&2
  exit 1
fi

export LD_LIBRARY_PATH="/usr/local/runtime/lib/aarch64:/usr/local/lib:${LD_LIBRARY_PATH:-}"
started_epoch="$(date +%s)"
before_throttled="$(vcgencmd get_throttled)"
stdbuf -oL -eL "$app" --duration "$duration" \
  --inference-backend "$backend" --model "$model" --inference-threads "$threads" \
  --events-dir "$events_dir" --event-min-free-bytes 1048576 >"$log" 2>&1 &
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
    temperature="$(vcgencmd measure_temp)"
    throttled="$(vcgencmd get_throttled)"
    echo "$timestamp rss_kb=$rss_kb fd_count=$fd_count $temperature $throttled"
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
after_throttled="$(vcgencmd get_throttled)"
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

final_line="$(grep '\[app\] stopped ' "$log" | tail -n 1)"
grep -q 'outstanding=0' <<<"$final_line" || {
  echo "soak test ended without clean lease release" >&2
  exit 1
}
for field in inference_copy_fallback inference_preprocess_errors \
  inference_dma_sync_errors inference_backend_errors capture_errors encoder_errors \
  encoder_recovery_failures rtsp_errors rtsp_recovery_failures events_failed; do
  grep -q "${field}=0" <<<"$final_line" || {
    echo "soak test failed nonzero ${field}: ${final_line}" >&2
    exit 1
  }
done
inferred="$(sed -n 's/.* inferred=\([0-9][0-9]*\).*/\1/p' <<<"$final_line")"
zero_copy="$(sed -n 's/.* inference_zero_copy=\([0-9][0-9]*\).*/\1/p' <<<"$final_line")"
[[ -n "$inferred" && "$inferred" -gt 0 && "$zero_copy" = "$inferred" ]] || {
  echo "soak test did not preserve 100% compact-I420 ingress: ${final_line}" >&2
  exit 1
}
for throttled in "$before_throttled" "$after_throttled"; do
  value="${throttled#throttled=0x}"
  (( (16#$value & 0xF) == 0 )) || {
    echo "current throttling detected: ${throttled}" >&2
    exit 1
  }
done
echo "soak test passed: backend=$backend model=$model log=$log " \
  "(resources: $resource_log, events: $events_dir)"
