#pragma once

#include "eggvision/frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace eggvision {

using DmaBufSyncOperation = int (*)(int fd, std::uint64_t flags, void *context);

// Brackets CPU reads from every unique DMA-BUF in a StreamView. The destructor
// ends all successfully-started sessions so exceptions cannot leave one active.
class DmaBufReadSync {
public:
    DmaBufReadSync(const StreamView &view, std::string &error);
    DmaBufReadSync(const StreamView &view,
                   std::string &error,
                   DmaBufSyncOperation operation,
                   void *context);
    ~DmaBufReadSync();

    DmaBufReadSync(const DmaBufReadSync &) = delete;
    DmaBufReadSync &operator=(const DmaBufReadSync &) = delete;

    explicit operator bool() const { return ready_; }
    bool finish(std::string &error);

private:
    int synchronize(int fd, std::uint64_t flags) const;

    DmaBufSyncOperation operation_ = nullptr;
    void *context_ = nullptr;
    std::vector<int> synchronized_fds_;
    bool ready_ = false;
};

}  // namespace eggvision
