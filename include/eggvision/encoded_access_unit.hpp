#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace eggvision {

// Application-owned representation of one encoded H.264 access unit. The
// payload is immutable and shared by the RTSP and event-recording consumers so
// compressed bytes are copied only once after leaving GStreamer.
struct EncodedAccessUnit {
    using Payload = std::vector<std::uint8_t>;

    std::uint64_t sensor_timestamp_ns = 0;
    std::uint64_t pts_ns = 0;
    std::uint64_t dts_ns = 0;
    std::uint64_t duration_ns = 0;
    std::uint64_t generation = 0;
    bool keyframe = false;
    bool has_sps = false;
    bool has_pps = false;
    std::shared_ptr<const Payload> payload;

    std::size_t sizeBytes() const { return payload ? payload->size() : 0; }
    bool independentlyDecodable() const { return keyframe && has_sps && has_pps; }
};

using EncodedAccessUnitPtr = std::shared_ptr<const EncodedAccessUnit>;

}  // namespace eggvision
