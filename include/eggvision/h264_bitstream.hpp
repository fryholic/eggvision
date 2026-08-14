#pragma once

#include <cstddef>
#include <cstdint>

namespace eggvision {

struct H264NalSummary {
    bool has_idr = false;
    bool has_sps = false;
    bool has_pps = false;
};

H264NalSummary inspectH264ByteStream(const std::uint8_t *data, std::size_t size);

}  // namespace eggvision
