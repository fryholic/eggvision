#include "eggvision/inference.hpp"
#include "eggvision/inference_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<float> readTensor(const std::string &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open FP32 tensor: " + path);
    }
    const auto byte_count = input.tellg();
    if (byte_count < 0 || byte_count % static_cast<std::streamoff>(sizeof(float)) != 0) {
        throw std::runtime_error("tensor is not a valid FP32 file: " + path);
    }
    std::vector<float> values(static_cast<std::size_t>(byte_count) / sizeof(float));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(values.data()), byte_count);
    if (!input) {
        throw std::runtime_error("cannot read FP32 tensor: " + path);
    }
    return values;
}

std::unique_ptr<eggvision::InferenceBackend> initializeBackend(const std::string &name,
                                                               const std::string &model_path,
                                                               unsigned threads) {
    eggvision::InferenceBackendConfig config;
    config.model_path = model_path;
    config.threads = threads;
    std::unique_ptr<eggvision::InferenceBackend> backend =
        eggvision::createInferenceBackend(name);
    std::string error;
    if (!backend->initialize(config, error)) {
        throw std::runtime_error(name + " initialization failed: " + error);
    }
    return backend;
}

eggvision::InferenceTensor runBackend(eggvision::InferenceBackend &backend,
                                      const std::vector<float> &input_values) {
    const eggvision::MutableInferenceTensor input = backend.inputTensor();
    if (!input || input.size != input_values.size()) {
        throw std::runtime_error(std::string(backend.name()) + " input contract mismatch");
    }
    std::copy(input_values.begin(), input_values.end(), input.data);
    std::string error;
    if (!backend.run(error)) {
        throw std::runtime_error(std::string(backend.name()) + " inference failed: " + error);
    }
    const eggvision::InferenceTensor output = backend.outputTensor();
    if (!output || output.rows != 6300 || output.fields != 85) {
        throw std::runtime_error(std::string(backend.name()) + " output contract mismatch");
    }
    return output;
}

std::vector<eggvision::Detection> decodePeople(const eggvision::InferenceTensor &output) {
    std::vector<eggvision::Detection> detections;
    for (std::size_t row_index = 0; row_index < output.rows; ++row_index) {
        const float *row = output.data + row_index * output.fields;
        const float confidence = row[4] * row[5];
        if (confidence < 0.30F) {
            continue;
        }
        detections.push_back({
            0,
            confidence,
            {row[0] - row[2] / 2.0F, row[1] - row[3] / 2.0F, row[2], row[3]},
        });
    }
    return eggvision::nonMaximumSuppression(std::move(detections), 0.45F);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        std::cerr << "usage: eggvision_inference_backend_compare "
                     "OPENVINO_MODEL MNN_MODEL INPUT_FP32 [MNN_REFERENCE_FP32]\n";
        return 2;
    }

    try {
        const std::vector<float> input_values = readTensor(argv[3]);
        auto openvino = initializeBackend("openvino", argv[1], 2);
        auto mnn = initializeBackend("mnn", argv[2], 3);
        const eggvision::InferenceTensor openvino_output = runBackend(*openvino, input_values);
        const eggvision::InferenceTensor mnn_output = runBackend(*mnn, input_values);

        const std::vector<eggvision::Detection> openvino_detections =
            decodePeople(openvino_output);
        const std::vector<eggvision::Detection> mnn_detections = decodePeople(mnn_output);
        float max_box_difference = 0.0F;
        float max_confidence_difference = 0.0F;
        if (openvino_detections.size() == mnn_detections.size()) {
            for (std::size_t index = 0; index < openvino_detections.size(); ++index) {
                const auto &left = openvino_detections[index];
                const auto &right = mnn_detections[index];
                max_box_difference = std::max(
                    {max_box_difference,
                     std::fabs(left.box.x - right.box.x),
                     std::fabs(left.box.y - right.box.y),
                     std::fabs(left.box.width - right.box.width),
                     std::fabs(left.box.height - right.box.height)});
                max_confidence_difference = std::max(
                    max_confidence_difference,
                    std::fabs(left.confidence - right.confidence));
            }
        }

        float reference_max_absolute = 0.0F;
        std::size_t reference_different_bits = 0;
        if (argc == 5) {
            const std::vector<float> reference = readTensor(argv[4]);
            const std::size_t output_size = mnn_output.rows * mnn_output.fields;
            if (reference.size() != output_size) {
                throw std::runtime_error("MNN reference output size mismatch");
            }
            for (std::size_t index = 0; index < output_size; ++index) {
                reference_max_absolute = std::max(
                    reference_max_absolute,
                    std::fabs(reference[index] - mnn_output.data[index]));
                if (std::memcmp(reference.data() + index,
                                mnn_output.data + index,
                                sizeof(float)) != 0) {
                    ++reference_different_bits;
                }
            }
        }

        const bool detections_match =
            openvino_detections.size() == mnn_detections.size() &&
            max_box_difference <= 1.0F && max_confidence_difference <= 0.005F;
        const bool reference_matches = argc != 5 || reference_max_absolute <= 1.0e-6F;
        std::cout << std::fixed << std::setprecision(9)
                  << "{\"openvino_detections\":" << openvino_detections.size()
                  << ",\"mnn_detections\":" << mnn_detections.size()
                  << ",\"max_box_difference\":" << max_box_difference
                  << ",\"max_confidence_difference\":" << max_confidence_difference
                  << ",\"mnn_reference_max_absolute\":" << reference_max_absolute
                  << ",\"mnn_reference_different_bits\":" << reference_different_bits
                  << "}\n";
        return detections_match && reference_matches ? 0 : 1;
    } catch (const std::exception &exception) {
        std::cerr << "inference backend comparison failed: " << exception.what() << '\n';
        return 1;
    }
}
