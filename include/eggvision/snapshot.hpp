#pragma once

#include "eggvision/i420.hpp"

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

// Stages the main profile while its FrameLease is alive. Every unique DMA-BUF
// fd is synchronized for CPU read access, copied, then released before return.
bool stageMainSnapshot(const FrameLease &frame, MainSnapshot &snapshot, std::string &error);

}  // namespace eggvision
