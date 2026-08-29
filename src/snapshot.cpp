#include "eggvision/snapshot.hpp"

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

}  // namespace

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
