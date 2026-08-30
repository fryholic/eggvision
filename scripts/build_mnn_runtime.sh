#!/usr/bin/env bash
set -euo pipefail

mnn_commit=d407447ed56c4121a11ccbd266dc184ca1ead0c2
if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 INSTALL_PREFIX [WORK_ROOT]" >&2
  exit 2
fi

install_prefix=$(realpath -m "$1")
work_root=$(realpath -m "${2:-build-mnn-3.6.1}")
source_root="${work_root}/source"
build_root="${work_root}/build"
mkdir -p "${work_root}"

if [[ ! -d "${source_root}/.git" ]]; then
  git clone --filter=blob:none --no-checkout https://github.com/alibaba/MNN.git \
    "${source_root}"
fi
if [[ -n "$(git -C "${source_root}" status --short)" ]]; then
  echo "MNN source tree is dirty: ${source_root}" >&2
  exit 1
fi
git -C "${source_root}" fetch --depth 1 origin "${mnn_commit}"
git -C "${source_root}" checkout --detach "${mnn_commit}"
test "$(git -C "${source_root}" rev-parse HEAD)" = "${mnn_commit}"

cmake -S "${source_root}" -B "${build_root}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${install_prefix}" \
  '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -mcpu=cortex-a72 -mtune=cortex-a72' \
  '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -mcpu=cortex-a72 -mtune=cortex-a72' \
  -DMNN_BUILD_SHARED_LIBS=OFF \
  -DMNN_BUILD_CONVERTER=OFF \
  -DMNN_BUILD_TOOLS=OFF \
  -DMNN_BUILD_TEST=OFF \
  -DMNN_BUILD_BENCHMARK=OFF \
  -DMNN_BUILD_DEMO=OFF \
  -DMNN_BUILD_TRAIN=OFF \
  -DMNN_BUILD_QUANTOOLS=OFF \
  -DMNN_BUILD_PROTOBUFFER=ON \
  -DMNN_OPENMP=OFF \
  -DMNN_USE_THREAD_POOL=ON \
  -DMNN_ARM82=OFF \
  -DMNN_KLEIDIAI=OFF \
  -DMNN_OPENCL=OFF \
  -DMNN_OPENGL=OFF \
  -DMNN_VULKAN=OFF \
  -DMNN_METAL=OFF
cmake --build "${build_root}" --parallel "$(nproc)"
cmake --install "${build_root}"

sha256sum "${install_prefix}/lib/libMNN.a"
echo "MNN 3.6.1 installed at ${install_prefix}"
