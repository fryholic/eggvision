#include "eggvision/snapshot.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <sstream>
#include <vector>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>

namespace eggvision {
namespace {

bool synchronize(int fd, std::uint64_t flags, std::string &error) {
    dma_buf_sync sync{};
    sync.flags = flags;
    while (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        if (errno == EINTR || errno == EAGAIN) {
            continue;
        }
        std::ostringstream message;
        message << "DMA_BUF_IOCTL_SYNC failed for fd " << fd << ": " << std::strerror(errno);
        error = message.str();
        return false;
    }
    return true;
}

void copyRows(std::uint8_t *target,
              const std::uint8_t *source,
              unsigned rows,
              unsigned width,
              unsigned stride) {
    for (unsigned row = 0; row < rows; ++row) {
        std::copy_n(source + static_cast<std::size_t>(row) * stride,
                    width,
                    target + static_cast<std::size_t>(row) * width);
    }
}

}  // namespace

bool copyMappedI420(const StreamView &view,
                    std::vector<std::uint8_t> &destination,
                    std::string &error) {
    error.clear();
    destination.clear();
    if (view.width == 0 || view.height == 0 || view.width % 2 != 0 ||
        view.height % 2 != 0 || view.stride < view.width || view.planes.empty()) {
        error = "invalid mapped I420 layout";
        return false;
    }

    const std::size_t y_size = static_cast<std::size_t>(view.width) * view.height;
    const std::size_t chroma_size = y_size / 4;
    destination.resize(y_size + 2 * chroma_size);

    if (view.planes.size() == 1 && view.planes[0].data) {
        const std::uint8_t *base = view.planes[0].data;
        const unsigned chroma_stride = view.stride / 2;
        const std::uint8_t *u = base + static_cast<std::size_t>(view.stride) * view.height;
        const std::uint8_t *v = u + static_cast<std::size_t>(chroma_stride) * (view.height / 2);
        copyRows(destination.data(), base, view.height, view.width, view.stride);
        copyRows(destination.data() + y_size,
                 u,
                 view.height / 2,
                 view.width / 2,
                 chroma_stride);
        copyRows(destination.data() + y_size + chroma_size,
                 v,
                 view.height / 2,
                 view.width / 2,
                 chroma_stride);
        return true;
    }
    if (view.planes.size() == 3 && view.planes[0].data && view.planes[1].data &&
        view.planes[2].data) {
        copyRows(destination.data(),
                 view.planes[0].data,
                 view.height,
                 view.width,
                 view.stride);
        copyRows(destination.data() + y_size,
                 view.planes[1].data,
                 view.height / 2,
                 view.width / 2,
                 view.stride / 2);
        copyRows(destination.data() + y_size + chroma_size,
                 view.planes[2].data,
                 view.height / 2,
                 view.width / 2,
                 view.stride / 2);
        return true;
    }
    error = "mapped I420 planes are unavailable";
    return false;
}

bool stageMainSnapshot(const FrameLease &frame, MainSnapshot &snapshot, std::string &error) {
    error.clear();
    snapshot = {};
    const StreamView &view = frame.main();
    std::set<int> unique_fds;
    for (const PlaneView &plane : view.planes) {
        if (plane.fd < 0 || !plane.data) {
            error = "main profile is not CPU mapped";
            return false;
        }
        unique_fds.insert(plane.fd);
    }

    std::vector<int> synchronized;
    synchronized.reserve(unique_fds.size());
    for (const int fd : unique_fds) {
        if (!synchronize(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ, error)) {
            for (auto it = synchronized.rbegin(); it != synchronized.rend(); ++it) {
                std::string ignored;
                synchronize(*it, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ, ignored);
            }
            return false;
        }
        synchronized.push_back(fd);
    }

    const bool copied = copyMappedI420(view, snapshot.i420, error);
    bool ended = true;
    for (auto it = synchronized.rbegin(); it != synchronized.rend(); ++it) {
        std::string sync_error;
        if (!synchronize(*it, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ, sync_error)) {
            if (error.empty()) {
                error = std::move(sync_error);
            }
            ended = false;
        }
    }
    if (!copied || !ended) {
        snapshot.i420.clear();
        return false;
    }

    snapshot.width = view.width;
    snapshot.height = view.height;
    snapshot.sequence = frame.sequence();
    snapshot.sensor_timestamp_ns = frame.sensorTimestampNs();
    return true;
}

}  // namespace eggvision
