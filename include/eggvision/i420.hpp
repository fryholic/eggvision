#pragma once

#include "eggvision/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eggvision {

enum class CompactI420Status {
    Compatible,
    InvalidDimensions,
    UnexpectedPlaneCount,
    UnexpectedStride,
    InvalidFileDescriptor,
    DifferentFileDescriptors,
    MissingMapping,
    InconsistentMapping,
    NonCompactOffsets,
    PlaneTooShort,
    MappingTooShort,
};

struct CompactI420View {
    CompactI420Status status = CompactI420Status::InvalidDimensions;
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;

    explicit operator bool() const { return status == CompactI420Status::Compatible; }
};

CompactI420View inspectCompactI420(const StreamView &view);
const char *compactI420StatusName(CompactI420Status status);

// Copies a mapped I420 view into tightly-packed Y/U/V planes. This helper is
// independent of DMA-BUF synchronization so stride handling can be unit tested.
bool copyMappedI420(const StreamView &view,
                    std::vector<std::uint8_t> &destination,
                    std::string &error);

}  // namespace eggvision
