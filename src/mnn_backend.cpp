#include "eggvision/inference_backend.hpp"

#include <MNN/Interpreter.hpp>
#include <MNN/MNNDefine.h>
#include <MNN/Tensor.hpp>

#if MNN_VERSION_MAJOR != 3 || MNN_VERSION_MINOR != 6 || MNN_VERSION_PATCH != 1
#error "EggVision requires the validated MNN 3.6.1 runtime"
#endif

#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace eggvision {
namespace {

std::vector<int> tensorShape(const MNN::Tensor &tensor) {
    std::vector<int> shape;
    shape.reserve(static_cast<std::size_t>(tensor.dimensions()));
    for (int dimension = 0; dimension < tensor.dimensions(); ++dimension) {
        shape.push_back(tensor.length(dimension));
    }
    return shape;
}

std::string shapeString(const std::vector<int> &shape) {
    std::ostringstream stream;
    stream << '[';
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (index != 0) {
            stream << ',';
        }
        stream << shape[index];
    }
    stream << ']';
    return stream.str();
}

bool isFp32(const MNN::Tensor &tensor) {
    const halide_type_t type = tensor.getType();
    return type.code == halide_type_float && type.bits == 32 && type.lanes == 1;
}

class MnnBackend final : public InferenceBackend {
public:
    ~MnnBackend() override {
        if (interpreter_ && session_) {
            interpreter_->releaseSession(session_);
        }
    }

    std::string_view name() const noexcept override {
        return "mnn";
    }

    bool initialize(const InferenceBackendConfig &config, std::string &error) override {
        try {
            interpreter_.reset(MNN::Interpreter::createFromFile(config.model_path.c_str()));
            if (!interpreter_) {
                error = "failed to load MNN model";
                return false;
            }

            backend_config_.precision = MNN::BackendConfig::Precision_Normal;
            backend_config_.power = MNN::BackendConfig::Power_High;
            backend_config_.memory = MNN::BackendConfig::Memory_Low;
            MNN::ScheduleConfig schedule;
            schedule.type = MNN_FORWARD_CPU;
            schedule.backupType = MNN_FORWARD_CPU;
            schedule.numThread = static_cast<int>(config.threads);
            schedule.backendConfig = &backend_config_;
            session_ = interpreter_->createSession(schedule);
            if (!session_) {
                error = "failed to create MNN CPU session";
                return false;
            }

            const auto &inputs = interpreter_->getSessionInputAll(session_);
            const auto &outputs = interpreter_->getSessionOutputAll(session_);
            if (inputs.size() != 1 || outputs.size() != 1) {
                error = "model must have exactly one input and one output";
                return false;
            }
            input_ = inputs.begin()->second;
            output_ = outputs.begin()->second;
            if (!input_ || !output_) {
                error = "MNN returned a null input or output tensor";
                return false;
            }

            const std::vector<int> input_shape = tensorShape(*input_);
            const std::vector<int> output_shape = tensorShape(*output_);
            const std::vector<int> expected_input{
                1,
                3,
                static_cast<int>(config.input_height),
                static_cast<int>(config.input_width),
            };
            const std::vector<int> expected_output{
                1,
                static_cast<int>(config.output_rows),
                static_cast<int>(config.output_fields),
            };
            if (!isFp32(*input_) || !isFp32(*output_)) {
                error = "model input and output must both be FP32";
                return false;
            }
            if (input_->getDimensionType() != MNN::Tensor::CAFFE ||
                output_->getDimensionType() != MNN::Tensor::CAFFE) {
                error = "model input and output must both use CAFFE/NCHW layout";
                return false;
            }
            if (input_shape != expected_input || output_shape != expected_output) {
                error = "unexpected model shapes input=" + shapeString(input_shape) +
                        " output=" + shapeString(output_shape) +
                        " expected_input=" + shapeString(expected_input) +
                        " expected_output=" + shapeString(expected_output);
                return false;
            }
            if (!input_->host<float>() || !output_->host<float>()) {
                error = "MNN CPU tensors are not directly host-accessible";
                return false;
            }

            input_size_ = static_cast<std::size_t>(input_->elementSize());
            output_rows_ = config.output_rows;
            output_fields_ = config.output_fields;
            return true;
        } catch (const std::exception &exception) {
            error = exception.what();
            return false;
        }
    }

    MutableInferenceTensor inputTensor() override {
        return {input_ ? input_->host<float>() : nullptr, input_size_};
    }

    bool run(std::string &error) override {
        const MNN::ErrorCode status = interpreter_->runSession(session_);
        if (status == MNN::NO_ERROR) {
            return true;
        }
        error = "MNN runSession returned status " + std::to_string(static_cast<int>(status));
        return false;
    }

    InferenceTensor outputTensor() override {
        return {output_ ? output_->host<float>() : nullptr, output_rows_, output_fields_};
    }

private:
    std::unique_ptr<MNN::Interpreter> interpreter_;
    MNN::Session *session_ = nullptr;
    MNN::Tensor *input_ = nullptr;
    MNN::Tensor *output_ = nullptr;
    MNN::BackendConfig backend_config_;
    std::size_t input_size_ = 0;
    std::size_t output_rows_ = 0;
    std::size_t output_fields_ = 0;
};

}  // namespace

std::unique_ptr<InferenceBackend> createMnnBackend() {
    return std::make_unique<MnnBackend>();
}

}  // namespace eggvision
