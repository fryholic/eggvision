#include "tensor_comparison.hpp"

#include <cmath>
#include <iostream>
#include <limits>

int main() {
    const float baseline[] = {0.0F, 1.0F, -2.0F};
    const ExactTensorComparison identical = compareExactTensors(baseline, baseline, 3);
    if (!identical.all_finite || identical.different_bits != 0) {
        std::cerr << "identical finite tensors were rejected\n";
        return 1;
    }

    const float one_ulp[] = {
        0.0F,
        std::nextafter(1.0F, std::numeric_limits<float>::infinity()),
        -2.0F,
    };
    if (compareExactTensors(baseline, one_ulp, 3).different_bits != 1) {
        std::cerr << "one-ULP mutation was not detected\n";
        return 1;
    }

    const float nan_values[] = {
        0.0F,
        std::numeric_limits<float>::quiet_NaN(),
        -2.0F,
    };
    if (compareExactTensors(nan_values, nan_values, 3).all_finite) {
        std::cerr << "NaN tensor was accepted as finite\n";
        return 1;
    }
    std::cout << "exact tensor comparison tests passed\n";
    return 0;
}
