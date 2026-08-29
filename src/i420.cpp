#include "eggvision/i420.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace eggvision {
namespace {

struct I420Sizes {
    std::size_t y = 0;
    std::size_t chroma = 0;
    std::size_t total = 0;
};

bool checkedMultiply(std::size_t left, std::size_t right, std::size_t &result) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool calculateSizes(const StreamView &view, I420Sizes &sizes) {
    if (view.width == 0 || view.height == 0 || view.width % 2 != 0 ||
        view.height % 2 != 0 ||
        !checkedMultiply(view.width, view.height, sizes.y)) {
        return false;
    }
    sizes.chroma = sizes.y / 4;
    if (sizes.chroma > (std::numeric_limits<std::size_t>::max() - sizes.y) / 2) {
        return false;
    }
    sizes.total = sizes.y + 2 * sizes.chroma;
    return true;
}

bool requiredRowSpan(unsigned rows,
                     unsigned width,
                     unsigned stride,
                     std::size_t &span) {
    if (rows == 0 || width == 0 || stride < width) {
        return false;
    }
    std::size_t row_offset = 0;
    if (!checkedMultiply(rows - 1, stride, row_offset) ||
        width > std::numeric_limits<std::size_t>::max() - row_offset) {
        return false;
    }
    span = row_offset + width;
    return true;
}

bool containsBytes(const PlaneView &plane, std::size_t required) {
    if (!plane.data || required > plane.length) {
        return false;
    }
    if (!plane.mapping_base) {
        return true;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(plane.mapping_base);
    const auto data = reinterpret_cast<std::uintptr_t>(plane.data);
    if (data < base || data - base > plane.mapped_length) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(data - base);
    return required <= plane.mapped_length - offset;
}

void copyRows(std::uint8_t *target,
              const std::uint8_t *source,
              unsigned rows,
              unsigned width,
              unsigned stride) {
    for (unsigned row = 0; row < rows; ++row) {
        std::copy_n(source + static_cast<std::size_t>(row) * stride,
                    width,
                    target + static_cast<std::size_t>(row) * width);
    }
}

}  // namespace

CompactI420View inspectCompactI420(const StreamView &view) {
    I420Sizes sizes;
    if (!calculateSizes(view, sizes)) {
        return {CompactI420Status::InvalidDimensions};
    }
    if (view.planes.size() != 3) {
        return {CompactI420Status::UnexpectedPlaneCount};
    }
    if (view.stride != view.width) {
        return {CompactI420Status::UnexpectedStride};
    }

    const PlaneView &y = view.planes[0];
    const PlaneView &u = view.planes[1];
    const PlaneView &v = view.planes[2];
    if (y.fd < 0 || u.fd < 0 || v.fd < 0) {
        return {CompactI420Status::InvalidFileDescriptor};
    }
    if (y.fd != u.fd || y.fd != v.fd) {
        return {CompactI420Status::DifferentFileDescriptors};
    }
    if (!y.data || !u.data || !v.data || !y.mapping_base || !u.mapping_base ||
        !v.mapping_base) {
        return {CompactI420Status::MissingMapping};
    }
    if (y.mapping_base != u.mapping_base || y.mapping_base != v.mapping_base ||
        y.mapped_length != u.mapped_length || y.mapped_length != v.mapped_length) {
        return {CompactI420Status::InconsistentMapping};
    }
    if (y.offset != 0 || u.offset != sizes.y || v.offset != sizes.y + sizes.chroma) {
        return {CompactI420Status::NonCompactOffsets};
    }
    if (y.length < sizes.y || u.length < sizes.chroma || v.length < sizes.chroma) {
        return {CompactI420Status::PlaneTooShort};
    }
    if (view.frame_size < sizes.total || y.mapped_length < sizes.total) {
        return {CompactI420Status::MappingTooShort};
    }
    if (y.data != y.mapping_base || u.data != y.mapping_base + sizes.y ||
        v.data != y.mapping_base + sizes.y + sizes.chroma) {
        return {CompactI420Status::InconsistentMapping};
    }
    return {CompactI420Status::Compatible, y.data, sizes.total};
}

const char *compactI420StatusName(CompactI420Status status) {
    switch (status) {
        case CompactI420Status::Compatible:
            return "compatible";
        case CompactI420Status::InvalidDimensions:
            return "invalid_dimensions";
        case CompactI420Status::UnexpectedPlaneCount:
            return "unexpected_plane_count";
        case CompactI420Status::UnexpectedStride:
            return "unexpected_stride";
        case CompactI420Status::InvalidFileDescriptor:
            return "invalid_file_descriptor";
        case CompactI420Status::DifferentFileDescriptors:
            return "different_file_descriptors";
        case CompactI420Status::MissingMapping:
            return "missing_mapping";
        case CompactI420Status::InconsistentMapping:
            return "inconsistent_mapping";
        case CompactI420Status::NonCompactOffsets:
            return "non_compact_offsets";
        case CompactI420Status::PlaneTooShort:
            return "plane_too_short";
        case CompactI420Status::MappingTooShort:
            return "mapping_too_short";
    }
    return "unknown";
}

bool copyMappedI420(const StreamView &view,
                    std::vector<std::uint8_t> &destination,
                    std::string &error) {
    error.clear();
    destination.clear();

    I420Sizes sizes;
    if (!calculateSizes(view, sizes) || view.stride < view.width || view.stride % 2 != 0 ||
        view.planes.empty()) {
        error = "invalid mapped I420 layout";
        return false;
    }

    std::size_t y_span = 0;
    std::size_t chroma_span = 0;
    if (!requiredRowSpan(view.height, view.width, view.stride, y_span) ||
        !requiredRowSpan(view.height / 2,
                         view.width / 2,
                         view.stride / 2,
                         chroma_span)) {
        error = "mapped I420 row span overflow";
        return false;
    }

    const std::uint8_t *y = nullptr;
    const std::uint8_t *u = nullptr;
    const std::uint8_t *v = nullptr;
    if (view.planes.size() == 1) {
        std::size_t y_block = 0;
        std::size_t chroma_block = 0;
        if (!checkedMultiply(view.stride, view.height, y_block) ||
            !checkedMultiply(view.stride / 2, view.height / 2, chroma_block) ||
            chroma_block > (std::numeric_limits<std::size_t>::max() - y_block) / 2) {
            error = "mapped I420 plane span overflow";
            return false;
        }
        const std::size_t required = y_block + 2 * chroma_block;
        if (!containsBytes(view.planes[0], required)) {
            error = "mapped I420 plane is shorter than its stride layout";
            return false;
        }
        y = view.planes[0].data;
        u = y + y_block;
        v = u + chroma_block;
    } else if (view.planes.size() == 3) {
        if (!containsBytes(view.planes[0], y_span) ||
            !containsBytes(view.planes[1], chroma_span) ||
            !containsBytes(view.planes[2], chroma_span)) {
            error = "mapped I420 plane is shorter than its row layout";
            return false;
        }
        y = view.planes[0].data;
        u = view.planes[1].data;
        v = view.planes[2].data;
    } else {
        error = "mapped I420 requires one or three planes";
        return false;
    }

    destination.resize(sizes.total);
    copyRows(destination.data(), y, view.height, view.width, view.stride);
    copyRows(destination.data() + sizes.y,
             u,
             view.height / 2,
             view.width / 2,
             view.stride / 2);
    copyRows(destination.data() + sizes.y + sizes.chroma,
             v,
             view.height / 2,
             view.width / 2,
             view.stride / 2);
    return true;
}

}  // namespace eggvision
