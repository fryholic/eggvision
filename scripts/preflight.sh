#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mnn_root="${MNN_ROOT:-/usr/local/eggvision/mnn-3.6.1}"

required_commands=(rpicam-hello gst-launch-1.0 gst-inspect-1.0 gst-discoverer-1.0 v4l2-ctl ar cmake od pkg-config)
for command in "${required_commands[@]}"; do
  command -v "$command" >/dev/null || { echo "missing command: $command" >&2; exit 1; }
done

for element in appsrc appsink queue v4l2h264enc h264parse rtph264pay mp4mux matroskamux filesink qtdemux; do
  gst-inspect-1.0 "$element" >/dev/null || { echo "missing GStreamer element: $element" >&2; exit 1; }
done

encoder_info="$(gst-inspect-1.0 v4l2h264enc)"
grep -q 'dmabuf-import' <<<"$encoder_info" || {
  echo "v4l2h264enc has no dmabuf-import mode" >&2
  exit 1
}

test -f /usr/local/runtime/cmake/OpenVINOConfig.cmake || {
  echo "OpenVINOConfig.cmake not found" >&2
  exit 1
}
test -f /usr/local/lib/cmake/opencv4/OpenCVConfig.cmake || {
  echo "OpenCVConfig.cmake not found" >&2
  exit 1
}
test -f "${mnn_root}/include/MNN/Interpreter.hpp" &&
  test -f "${mnn_root}/lib/libMNN.a" || {
  echo "MNN 3.6.1 runtime not found under ${mnn_root}" >&2
  exit 1
}
mnn_archive="${mnn_root}/lib/libMNN.a"
mnn_archive_magic="$(od -An -tx1 -N8 "${mnn_archive}" | tr -d '[:space:]')"
if [[ "${mnn_archive_magic}" != "213c617263683e0a" ]] ||
  ! ar -t "${mnn_archive}" >/dev/null 2>&1; then
  echo "MNN runtime is not a regular static archive: ${mnn_archive}" >&2
  exit 1
fi
test -f models/yolov5n.xml && test -f models/yolov5n.bin || {
  echo "models/yolov5n.xml and .bin are required" >&2
  exit 1
}
test -f models/yolov5n.mnn || {
  echo "models/yolov5n.mnn is required" >&2
  exit 1
}

rpicam-hello --list-cameras
rpicam-hello --nopreview --timeout 3000
v4l2-ctl -d /dev/video11 --list-ctrls-menus | grep -E 'h264_level|h264_profile|video_bitrate'

echo "preflight passed"
