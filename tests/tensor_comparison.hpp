#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

struct ExactTensorComparison {
    double absolute_sum = 0.0;
    float max_absolute = 0.0F;
    std::size_t different_bits = 0;
    bool all_finite = true;
};

inline ExactTensorComparison compareExactTensors(const float *left,
                                                 const float *right,
                                                 std::size_t size) {
    ExactTensorComparison result;
    for (std::size_t index = 0; index < size; ++index) {
        const bool finite = std::isfinite(left[index]) && std::isfinite(right[index]);
        result.all_finite = result.all_finite && finite;
        if (finite) {
            const float absolute = std::fabs(left[index] - right[index]);
            result.absolute_sum += absolute;
            result.max_absolute = std::max(result.max_absolute, absolute);
        }
        if (std::memcmp(left + index, right + index, sizeof(float)) != 0) {
            ++result.different_bits;
        }
    }
    return result;
}
