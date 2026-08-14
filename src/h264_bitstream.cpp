#include "eggvision/h264_bitstream.hpp"

namespace eggvision {
namespace {

std::size_t startCodeLength(const std::uint8_t *data, std::size_t size, std::size_t offset) {
    if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 1) {
        return 3;
    }
    if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 0 && data[offset + 3] == 1) {
        return 4;
    }
    return 0;
}

}  // namespace

H264NalSummary inspectH264ByteStream(const std::uint8_t *data, std::size_t size) {
    H264NalSummary summary;
    if (!data || size < 4) {
        return summary;
    }

    for (std::size_t offset = 0; offset + 3 < size;) {
        const std::size_t prefix = startCodeLength(data, size, offset);
        if (prefix == 0) {
            ++offset;
            continue;
        }
        const std::size_t header = offset + prefix;
        if (header >= size) {
            break;
        }
        const std::uint8_t type = data[header] & 0x1f;
        summary.has_idr = summary.has_idr || type == 5;
        summary.has_sps = summary.has_sps || type == 7;
        summary.has_pps = summary.has_pps || type == 8;
        offset = header + 1;
    }
    return summary;
}

}  // namespace eggvision
