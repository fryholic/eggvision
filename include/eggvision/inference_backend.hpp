#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace eggvision {

struct InferenceBackendConfig {
    std::string model_path;
    unsigned input_width = 320;
    unsigned input_height = 320;
    std::size_t output_rows = 6300;
    std::size_t output_fields = 85;
    unsigned threads = 2;
};

struct MutableInferenceTensor {
    float *data = nullptr;
    std::size_t size = 0;

    explicit operator bool() const noexcept {
        return data != nullptr && size != 0;
    }
};

struct InferenceTensor {
    const float *data = nullptr;
    std::size_t rows = 0;
    std::size_t fields = 0;

    explicit operator bool() const noexcept {
        return data != nullptr && rows != 0 && fields != 0;
    }
};

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual bool initialize(const InferenceBackendConfig &config, std::string &error) = 0;
    virtual MutableInferenceTensor inputTensor() = 0;
    virtual bool run(std::string &error) = 0;
    virtual InferenceTensor outputTensor() = 0;
};

std::unique_ptr<InferenceBackend> createInferenceBackend(std::string_view name);

}  // namespace eggvision
