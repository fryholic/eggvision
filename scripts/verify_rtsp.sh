#!/usr/bin/env bash
set -euo pipefail

url="${1:-rtsp://127.0.0.1:8554/stream}"
attempts="${2:-3}"
seconds="${3:-10}"

for ((attempt = 1; attempt <= attempts; ++attempt)); do
  protocol=tcp
  if (( attempt == attempts )); then
    protocol=udp
  fi
  set +e
  timeout --signal=INT "$seconds" gst-launch-1.0 -q \
    rtspsrc location="$url" protocols="$protocol" latency=100 \
    ! rtph264depay ! h264parse ! fakesink sync=false
  status=$?
  set -e
  if [[ $status -ne 0 && $status -ne 124 && $status -ne 130 ]]; then
    echo "RTSP attempt $attempt failed with status $status" >&2
    exit 1
  fi
  echo "RTSP attempt $attempt/$attempts passed using $protocol"
done

