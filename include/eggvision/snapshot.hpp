#pragma once

#include "eggvision/frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace eggvision {

struct MainSnapshot {
    unsigned width = 0;
    unsigned height = 0;
    std::uint64_t sequence = 0;
    std::uint64_t sensor_timestamp_ns = 0;
    std::vector<std::uint8_t> i420;
};

// Copies a mapped I420 view into tightly-packed Y/U/V planes. This helper is
// independent of DMA-BUF synchronization so stride handling can be unit tested.
bool copyMappedI420(const StreamView &view,
                    std::vector<std::uint8_t> &destination,
                    std::string &error);

// Stages the main profile while its FrameLease is alive. Every unique DMA-BUF
// fd is synchronized for CPU read access, copied, then released before return.
bool stageMainSnapshot(const FrameLease &frame, MainSnapshot &snapshot, std::string &error);

}  // namespace eggvision
