#include "eggvision/inference_backend.hpp"

#include <openvino/openvino.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<float> readInput(const std::string &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open input tensor: " + path);
    }
    const auto byte_count = input.tellg();
    if (byte_count < 0 || byte_count % static_cast<std::streamoff>(sizeof(float)) != 0) {
        throw std::runtime_error("input tensor is not an FP32 file");
    }
    std::vector<float> values(static_cast<std::size_t>(byte_count) / sizeof(float));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(values.data()), byte_count);
    if (!input) {
        throw std::runtime_error("cannot read input tensor: " + path);
    }
    return values;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: eggvision_openvino_backend_parity MODEL INPUT_FP32 [THREADS]\n";
        return 2;
    }

    try {
        const std::string model_path = argv[1];
        const std::vector<float> input_values = readInput(argv[2]);
        const unsigned threads = argc == 4 ? static_cast<unsigned>(std::stoul(argv[3])) : 2;

        eggvision::InferenceBackendConfig config;
        config.model_path = model_path;
        config.threads = threads;
        std::unique_ptr<eggvision::InferenceBackend> backend =
            eggvision::createInferenceBackend("openvino");
        std::string error;
        if (!backend->initialize(config, error)) {
            throw std::runtime_error("adapter initialization failed: " + error);
        }
        const eggvision::MutableInferenceTensor adapter_input = backend->inputTensor();
        if (!adapter_input || adapter_input.size != input_values.size()) {
            throw std::runtime_error("adapter input contract mismatch");
        }
        std::copy(input_values.begin(), input_values.end(), adapter_input.data);
        if (!backend->run(error)) {
            throw std::runtime_error("adapter inference failed: " + error);
        }
        const eggvision::InferenceTensor adapter_output = backend->outputTensor();

        ov::Core core;
        std::shared_ptr<ov::Model> model = core.read_model(model_path);
        ov::AnyMap properties{
            {ov::hint::performance_mode.name(), ov::hint::PerformanceMode::LATENCY},
            {ov::inference_num_threads.name(), static_cast<int>(threads)},
        };
        ov::CompiledModel compiled = core.compile_model(model, "CPU", properties);
        ov::InferRequest request = compiled.create_infer_request();
        ov::Tensor legacy_input = request.get_input_tensor();
        if (legacy_input.get_size() != input_values.size()) {
            throw std::runtime_error("legacy input contract mismatch");
        }
        std::copy(input_values.begin(), input_values.end(), legacy_input.data<float>());
        request.infer();
        ov::Tensor legacy_output = request.get_output_tensor();

        const std::size_t adapter_size = adapter_output.rows * adapter_output.fields;
        if (!adapter_output || adapter_size != legacy_output.get_size()) {
            throw std::runtime_error("adapter output contract mismatch");
        }
        const float *legacy_data = legacy_output.data<float>();
        double absolute_sum = 0.0;
        float max_absolute = 0.0F;
        std::size_t different_bits = 0;
        for (std::size_t i = 0; i < adapter_size; ++i) {
            const float absolute = std::fabs(adapter_output.data[i] - legacy_data[i]);
            absolute_sum += absolute;
            max_absolute = std::max(max_absolute, absolute);
            if (std::memcmp(adapter_output.data + i, legacy_data + i, sizeof(float)) != 0) {
                ++different_bits;
            }
        }

        std::cout << std::fixed << std::setprecision(9)
                  << "{\"backend\":\"openvino\",\"elements\":" << adapter_size
                  << ",\"max_absolute\":" << max_absolute
                  << ",\"mean_absolute\":" << absolute_sum / adapter_size
                  << ",\"different_bits\":" << different_bits << "}\n";
        return max_absolute <= 1.0e-6F ? 0 : 1;
    } catch (const std::exception &exception) {
        std::cerr << "openvino backend parity failed: " << exception.what() << '\n';
        return 1;
    }
}
