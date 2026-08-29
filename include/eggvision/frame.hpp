#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <libcamera/framebuffer.h>
#include <libcamera/request.h>

namespace eggvision {

struct PlaneView {
    int fd = -1;                  // Borrowed from libcamera for the lease lifetime.
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t bytes_used = 0;
    const std::uint8_t *data = nullptr;  // Only populated for the mapped lores stream.
    const std::uint8_t *mapping_base = nullptr;
    std::size_t mapped_length = 0;
};

struct StreamView {
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride = 0;
    unsigned frame_size = 0;
    std::vector<PlaneView> planes;
};

class FrameLease {
public:
    using Release = std::function<void(libcamera::Request *)>;

    FrameLease(libcamera::Request *request,
               StreamView main,
               StreamView lores,
               std::uint64_t sequence,
               std::uint64_t sensor_timestamp_ns,
               Release release)
        : request_(request),
          main_(std::move(main)),
          lores_(std::move(lores)),
          sequence_(sequence),
          sensor_timestamp_ns_(sensor_timestamp_ns),
          release_(std::move(release)) {}

    ~FrameLease() {
        if (release_) {
            release_(request_);
        }
    }

    FrameLease(const FrameLease &) = delete;
    FrameLease &operator=(const FrameLease &) = delete;

    const StreamView &main() const { return main_; }
    const StreamView &lores() const { return lores_; }
    std::uint64_t sequence() const { return sequence_; }
    std::uint64_t sensorTimestampNs() const { return sensor_timestamp_ns_; }

private:
    libcamera::Request *request_;
    StreamView main_;
    StreamView lores_;
    std::uint64_t sequence_;
    std::uint64_t sensor_timestamp_ns_;
    Release release_;
};

}  // namespace eggvision

