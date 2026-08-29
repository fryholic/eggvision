#include "eggvision/inference.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <openvino/openvino.hpp>

namespace {

struct Difference {
    std::size_t count = 0;
    double sum_absolute = 0.0;
    float max_absolute = 0.0F;
    std::size_t max_index = 0;
    float max_left = 0.0F;
    float max_right = 0.0F;
};

void divideReference(const cv::Mat &bgr, float *destination) {
    const std::size_t plane_size = static_cast<std::size_t>(bgr.cols) * bgr.rows;
    for (int y = 0; y < bgr.rows; ++y) {
        const auto *row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * bgr.cols + x;
            destination[index] = row[x][2] / 255.0F;
            destination[plane_size + index] = row[x][1] / 255.0F;
            destination[2 * plane_size + index] = row[x][0] / 255.0F;
        }
    }
}

std::uint32_t floatBits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint32_t positiveUlpDistance(float left, float right) {
    const std::uint32_t left_bits = floatBits(left);
    const std::uint32_t right_bits = floatBits(right);
    return left_bits > right_bits ? left_bits - right_bits : right_bits - left_bits;
}

Difference compareValues(const float *left, const float *right, std::size_t count) {
    Difference difference;
    for (std::size_t index = 0; index < count; ++index) {
        if (!std::isfinite(left[index]) || !std::isfinite(right[index])) {
            throw std::runtime_error("non-finite tensor value");
        }
        const float absolute = std::fabs(left[index] - right[index]);
        if (absolute != 0.0F) {
            ++difference.count;
        }
        difference.sum_absolute += absolute;
        if (absolute > difference.max_absolute) {
            difference.max_absolute = absolute;
            difference.max_index = index;
            difference.max_left = left[index];
            difference.max_right = right[index];
        }
    }
    return difference;
}

std::vector<eggvision::Detection> decode(ov::Tensor output,
                                         const eggvision::LetterboxTransform &transform,
                                         int source_width,
                                         int source_height,
                                         float confidence_threshold,
                                         float nms_threshold) {
    const auto shape = output.get_shape();
    if (shape.size() != 3 || shape[0] != 1 || shape[2] < 6) {
        throw std::runtime_error("unexpected output shape");
    }
    const std::size_t fields = shape[2];
    const float *data = output.data<float>();
    std::vector<eggvision::Detection> detections;
    for (std::size_t row_index = 0; row_index < shape[1]; ++row_index) {
        const float *row = data + row_index * fields;
        const float confidence = row[4] * row[5];
        if (confidence < confidence_threshold) {
            continue;
        }
        const cv::Rect2f model_box(row[0] - row[2] / 2.0F,
                                   row[1] - row[3] / 2.0F,
                                   row[2],
                                   row[3]);
        cv::Rect2f restored = eggvision::restoreLetterboxBox(
            model_box, transform, source_width, source_height);
        if (restored.width > 0.0F && restored.height > 0.0F) {
            detections.push_back({0, confidence, restored});
        }
    }
    return eggvision::nonMaximumSuppression(std::move(detections), nms_threshold);
}

struct ThresholdComparison {
    std::size_t classification_changes = 0;
    std::size_t nearest_row = 0;
    float divide_confidence = 0.0F;
    float production_confidence = 0.0F;
    float nearest_distance = std::numeric_limits<float>::infinity();
    float confidence_max_absolute = 0.0F;
};

ThresholdComparison compareThresholdRows(ov::Tensor divide_output,
                                         ov::Tensor production_output,
                                         float confidence_threshold) {
    const auto shape = divide_output.get_shape();
    if (shape != production_output.get_shape() || shape.size() != 3 || shape[2] < 6) {
        throw std::runtime_error("output shapes differ");
    }
    const std::size_t fields = shape[2];
    const float *divide_data = divide_output.data<float>();
    const float *production_data = production_output.data<float>();
    ThresholdComparison result;
    for (std::size_t row_index = 0; row_index < shape[1]; ++row_index) {
        const float divide_confidence =
            divide_data[row_index * fields + 4] * divide_data[row_index * fields + 5];
        const float production_confidence =
            production_data[row_index * fields + 4] * production_data[row_index * fields + 5];
        result.confidence_max_absolute = std::max(
            result.confidence_max_absolute,
            std::fabs(divide_confidence - production_confidence));
        if ((divide_confidence >= confidence_threshold) !=
            (production_confidence >= confidence_threshold)) {
            ++result.classification_changes;
        }
        const float distance = std::fabs(divide_confidence - confidence_threshold);
        if (distance < result.nearest_distance) {
            result.nearest_distance = distance;
            result.nearest_row = row_index;
            result.divide_confidence = divide_confidence;
            result.production_confidence = production_confidence;
        }
    }
    return result;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: eggvision_normalization_model_compare MODEL IMAGE "
                     "[CONFIDENCE] [THREADS]\n";
        return 2;
    }

    try {
        const std::string model_path = argv[1];
        const std::string image_path = argv[2];
        const float confidence_threshold = argc > 3 ? std::stof(argv[3]) : 0.30F;
        const int threads = argc > 4 ? std::stoi(argv[4]) : 2;
        if (confidence_threshold < 0.0F || confidence_threshold > 1.0F || threads <= 0) {
            throw std::invalid_argument("invalid confidence or thread count");
        }

        cv::Mat source = cv::imread(image_path, cv::IMREAD_COLOR);
        if (source.empty()) {
            throw std::runtime_error("failed to load comparison image");
        }

        constexpr int kInputWidth = 320;
        constexpr int kInputHeight = 320;
        const auto transform = eggvision::calculateLetterbox(
            source.cols, source.rows, kInputWidth, kInputHeight);
        cv::Mat resized;
        cv::resize(source,
                   resized,
                   cv::Size(transform.resized_width, transform.resized_height));
        cv::Mat letterboxed(kInputHeight,
                            kInputWidth,
                            CV_8UC3,
                            cv::Scalar(114, 114, 114));
        resized.copyTo(letterboxed(cv::Rect(transform.pad_x,
                                            transform.pad_y,
                                            transform.resized_width,
                                            transform.resized_height)));

        ov::Core core;
        auto model = core.read_model(model_path);
        const ov::Shape expected_input{1, 3, kInputHeight, kInputWidth};
        if (model->input().get_shape() != expected_input) {
            throw std::runtime_error("model input is not 1x3x320x320");
        }
        ov::AnyMap properties{
            {ov::hint::performance_mode.name(), ov::hint::PerformanceMode::LATENCY},
            {ov::inference_num_threads.name(), threads},
        };
        auto compiled = core.compile_model(model, "CPU", properties);
        auto divide_request = compiled.create_infer_request();
        auto production_request = compiled.create_infer_request();
        ov::Tensor divide_input = divide_request.get_input_tensor();
        ov::Tensor production_input = production_request.get_input_tensor();
        divideReference(letterboxed, divide_input.data<float>());
        eggvision::bgrToNormalizedRgbChw(letterboxed, production_input.data<float>());

        const std::size_t input_count = divide_input.get_size();
        Difference input_difference = compareValues(divide_input.data<float>(),
                                                    production_input.data<float>(),
                                                    input_count);
        std::uint32_t input_max_ulp = 0;
        for (std::size_t index = 0; index < input_count; ++index) {
            input_max_ulp = std::max(
                input_max_ulp,
                positiveUlpDistance(divide_input.data<float>()[index],
                                    production_input.data<float>()[index]));
        }

        divide_request.infer();
        production_request.infer();
        ov::Tensor divide_output = divide_request.get_output_tensor();
        ov::Tensor production_output = production_request.get_output_tensor();
        if (divide_output.get_shape() != production_output.get_shape()) {
            throw std::runtime_error("inference output shapes differ");
        }
        const std::size_t output_count = divide_output.get_size();
        Difference output_difference = compareValues(divide_output.data<float>(),
                                                      production_output.data<float>(),
                                                      output_count);
        ThresholdComparison threshold = compareThresholdRows(
            divide_output, production_output, confidence_threshold);
        const auto divide_detections = decode(divide_output,
                                              transform,
                                              source.cols,
                                              source.rows,
                                              confidence_threshold,
                                              0.45F);
        const auto production_detections = decode(production_output,
                                                  transform,
                                                  source.cols,
                                                  source.rows,
                                                  confidence_threshold,
                                                  0.45F);

        float detection_confidence_max_absolute = 0.0F;
        float detection_box_max_absolute = 0.0F;
        bool detection_identity = divide_detections.size() == production_detections.size();
        if (detection_identity) {
            for (std::size_t index = 0; index < divide_detections.size(); ++index) {
                const auto &left = divide_detections[index];
                const auto &right = production_detections[index];
                detection_identity = detection_identity && left.class_id == right.class_id;
                detection_confidence_max_absolute = std::max(
                    detection_confidence_max_absolute,
                    std::fabs(left.confidence - right.confidence));
                detection_box_max_absolute = std::max({
                    detection_box_max_absolute,
                    std::fabs(left.box.x - right.box.x),
                    std::fabs(left.box.y - right.box.y),
                    std::fabs(left.box.width - right.box.width),
                    std::fabs(left.box.height - right.box.height),
                });
            }
        }

        std::cout << std::setprecision(10)
                  << "image_width=" << source.cols << '\n'
                  << "image_height=" << source.rows << '\n'
                  << "input_values=" << input_count << '\n'
                  << "input_different_values=" << input_difference.count << '\n'
                  << "input_max_ulp=" << input_max_ulp << '\n'
                  << "input_max_absolute=" << input_difference.max_absolute << '\n'
                  << "input_mean_absolute="
                  << input_difference.sum_absolute / input_count << '\n'
                  << "output_values=" << output_count << '\n'
                  << "output_different_values=" << output_difference.count << '\n'
                  << "output_max_absolute=" << output_difference.max_absolute << '\n'
                  << "output_max_index=" << output_difference.max_index << '\n'
                  << "output_max_field="
                  << output_difference.max_index % divide_output.get_shape()[2] << '\n'
                  << "output_max_divide_value=" << output_difference.max_left << '\n'
                  << "output_max_production_value=" << output_difference.max_right << '\n'
                  << "output_mean_absolute="
                  << output_difference.sum_absolute / output_count << '\n'
                  << "threshold=" << confidence_threshold << '\n'
                  << "threshold_classification_changes=" << threshold.classification_changes
                  << '\n'
                  << "threshold_nearest_row=" << threshold.nearest_row << '\n'
                  << "threshold_nearest_divide_confidence=" << threshold.divide_confidence
                  << '\n'
                  << "threshold_nearest_production_confidence="
                  << threshold.production_confidence << '\n'
                  << "confidence_max_absolute=" << threshold.confidence_max_absolute << '\n'
                  << "divide_detection_count=" << divide_detections.size() << '\n'
                  << "production_detection_count=" << production_detections.size() << '\n'
                  << "detection_confidence_max_absolute="
                  << detection_confidence_max_absolute << '\n'
                  << "detection_box_max_absolute=" << detection_box_max_absolute << '\n';

        const bool passed =
            input_max_ulp <= 1 &&
            input_difference.max_absolute <= std::numeric_limits<float>::epsilon() / 2.0F &&
            output_difference.max_absolute <= 1.0e-2F &&
            threshold.confidence_max_absolute <= 1.0e-5F &&
            threshold.classification_changes == 0 && detection_identity &&
            detection_confidence_max_absolute <= 1.0e-5F &&
            detection_box_max_absolute <= 1.0e-3F;
        std::cout << "comparison=" << (passed ? "PASS" : "FAIL") << '\n';
        return passed ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "normalization model comparison failed: " << error.what() << '\n';
        return 1;
    }
}
