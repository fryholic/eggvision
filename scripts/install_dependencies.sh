#!/usr/bin/env bash
set -euo pipefail

apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake git pkg-config v4l-utils \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-base-apps \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstrtspserver-1.0-dev

echo "Dependencies installed. libcamera, OpenVINO and OpenCV are intentionally not replaced."
echo "Build the pinned MNN runtime separately with scripts/build_mnn_runtime.sh."

