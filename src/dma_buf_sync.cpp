#include "eggvision/dma_buf_sync.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>

namespace eggvision {
namespace {

int systemSync(int fd, std::uint64_t flags, void *) {
    dma_buf_sync sync{};
    sync.flags = flags;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0) {
        return 0;
    }
    return errno;
}

std::string syncError(const char *phase, int fd, int error_number) {
    std::ostringstream message;
    message << "DMA_BUF_IOCTL_SYNC " << phase << " failed for fd " << fd << ": "
            << std::strerror(error_number);
    return message.str();
}

}  // namespace

DmaBufReadSync::DmaBufReadSync(const StreamView &view, std::string &error)
    : DmaBufReadSync(view, error, systemSync, nullptr) {}

DmaBufReadSync::DmaBufReadSync(const StreamView &view,
                               std::string &error,
                               DmaBufSyncOperation operation,
                               void *context)
    : operation_(operation), context_(context) {
    error.clear();
    if (!operation_) {
        error = "DMA-BUF sync operation is unavailable";
        return;
    }

    std::vector<int> unique_fds;
    unique_fds.reserve(view.planes.size());
    for (const PlaneView &plane : view.planes) {
        if (plane.fd < 0) {
            error = "DMA-BUF sync requires valid plane file descriptors";
            return;
        }
        if (std::find(unique_fds.begin(), unique_fds.end(), plane.fd) == unique_fds.end()) {
            unique_fds.push_back(plane.fd);
        }
    }
    if (unique_fds.empty()) {
        error = "DMA-BUF sync requires at least one plane";
        return;
    }

    synchronized_fds_.reserve(unique_fds.size());
    for (const int fd : unique_fds) {
        const int result = synchronize(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        if (result != 0) {
            error = syncError("START", fd, result);
            std::string cleanup_error;
            finish(cleanup_error);
            if (!cleanup_error.empty()) {
                error += "; cleanup: " + cleanup_error;
            }
            return;
        }
        synchronized_fds_.push_back(fd);
    }
    ready_ = true;
}

DmaBufReadSync::~DmaBufReadSync() {
    std::string ignored;
    finish(ignored);
}

int DmaBufReadSync::synchronize(int fd, std::uint64_t flags) const {
    for (;;) {
        const int result = operation_(fd, flags, context_);
        if (result != EINTR && result != EAGAIN) {
            return result;
        }
    }
}

bool DmaBufReadSync::finish(std::string &error) {
    error.clear();
    bool success = true;
    for (auto it = synchronized_fds_.rbegin(); it != synchronized_fds_.rend(); ++it) {
        const int result = synchronize(*it, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        if (result != 0 && success) {
            error = syncError("END", *it, result);
            success = false;
        }
    }
    synchronized_fds_.clear();
    ready_ = false;
    return success;
}

}  // namespace eggvision
