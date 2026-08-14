#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

output_root="${1:?usage: event_rtsp_integration_test.sh NEW_OUTPUT_DIR [CLIENTS]}"
client_count="${2:-2}"
if [[ ! "$client_count" =~ ^[0-2]$ ]]; then
  echo "CLIENTS must be 0, 1, or 2" >&2
  exit 1
fi
if [[ -e "$output_root" ]]; then
  echo "output path already exists: $output_root" >&2
  exit 1
fi

log="${output_root}.log"
export LD_LIBRARY_PATH="/usr/local/runtime/lib/aarch64:/usr/local/lib:${LD_LIBRARY_PATH:-}"
export EGGVISION_EVENT_TEST_TRIGGER_AFTER_MS=2000

./build/eggvision_app \
  --events-dir "$output_root" \
  --event-min-free-bytes 1048576 \
  --duration 10 \
  --no-inference >"$log" 2>&1 &
app_pid=$!
client_pids=()
cleanup() {
  kill -TERM "$app_pid" 2>/dev/null || true
  for pid in "${client_pids[@]}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

for _ in {1..100}; do
  if ss -ltn | grep -q ':8554 '; then
    break
  fi
  if ! kill -0 "$app_pid" 2>/dev/null; then
    echo "application exited before RTSP became ready" >&2
    cat "$log" >&2
    exit 1
  fi
  sleep 0.1
done
ss -ltn | grep -q ':8554 ' || { echo "RTSP startup timed out" >&2; exit 1; }
sleep 0.2

for ((index = 0; index < client_count; ++index)); do
  timeout --signal=INT 6 gst-launch-1.0 -q \
    rtspsrc location=rtsp://127.0.0.1:8554/stream protocols=tcp latency=100 \
    ! rtph264depay ! h264parse ! fakesink sync=false &
  client_pids+=("$!")
done
for pid in "${client_pids[@]}"; do
  set +e
  wait "$pid"
  status=$?
  set -e
  if [[ $status -ne 0 && $status -ne 124 && $status -ne 130 ]]; then
    echo "RTSP client failed with status $status" >&2
    exit 1
  fi
done

wait "$app_pid"
trap - EXIT
grep -q '"events_completed":1' "$log"
grep -q '\[app\] stopped .*outstanding=0' "$log"
grep -q 'encoder_errors=0' "$log"

mapfile -t event_dirs < <(find "$output_root" -mindepth 2 -maxdepth 2 -type d ! -name '.*')
if [[ ${#event_dirs[@]} -ne 1 ]]; then
  echo "expected exactly one finalized event, got ${#event_dirs[@]}" >&2
  exit 1
fi
event_dir="${event_dirs[0]}"
test -s "$event_dir/snapshot.jpg"
test -s "$event_dir/clip.mp4"
test -s "$event_dir/metadata.json"
file "$event_dir/snapshot.jpg" | grep -q '1920x1080'
grep -q '"pre_roll_complete": true' "$event_dir/metadata.json"
grep -q '"post_roll_complete": true' "$event_dir/metadata.json"

gst-launch-1.0 -q filesrc location="$event_dir/clip.mp4" \
  ! qtdemux ! h264parse ! fakesink
gst-discoverer-1.0 "$event_dir/clip.mp4" >/dev/null
codec="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
  -of default=noprint_wrappers=1:nokey=1 "$event_dir/clip.mp4")"
profile="$(ffprobe -v error -select_streams v:0 -show_entries stream=profile \
  -of default=noprint_wrappers=1:nokey=1 "$event_dir/clip.mp4")"
dimensions="$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
  -of csv=p=0:s=x "$event_dir/clip.mp4")"
duration="$(ffprobe -v error -select_streams v:0 -show_entries stream=duration \
  -of default=noprint_wrappers=1:nokey=1 "$event_dir/clip.mp4")"
[[ "$codec" == "h264" && "$profile" == "High" && "$dimensions" == "1920x1080" ]]
awk -v duration="$duration" 'BEGIN { exit !(duration >= 3.0 && duration <= 3.5) }'

echo "event RTSP integration passed: clients=$client_count duration=$duration event=$event_dir"
