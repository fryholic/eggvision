#include "eggvision/inference_backend.hpp"

#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include <openvino/openvino.hpp>

namespace eggvision {
namespace {

std::string shapeString(const ov::Shape &shape) {
    std::ostringstream stream;
    stream << shape;
    return stream.str();
}

class OpenVinoBackend final : public InferenceBackend {
public:
    std::string_view name() const noexcept override {
        return "openvino";
    }

    bool initialize(const InferenceBackendConfig &config, std::string &error) override {
        try {
            model_ = core_.read_model(config.model_path);
            if (model_->inputs().size() != 1 || model_->outputs().size() != 1) {
                error = "model must have exactly one input and one output";
                return false;
            }

            const ov::Output<const ov::Node> input_port = model_->input();
            const ov::Output<const ov::Node> output_port = model_->output();
            const ov::Shape input_shape = input_port.get_shape();
            const ov::Shape output_shape = output_port.get_shape();
            const ov::Shape expected_input{1, 3, config.input_height, config.input_width};
            const ov::Shape expected_output{1, config.output_rows, config.output_fields};
            if (input_port.get_element_type() != ov::element::f32 ||
                output_port.get_element_type() != ov::element::f32) {
                error = "model input and output must both be FP32";
                return false;
            }
            if (input_shape != expected_input || output_shape != expected_output) {
                error = "unexpected model shapes input=" + shapeString(input_shape) +
                        " output=" + shapeString(output_shape) +
                        " expected_input=" + shapeString(expected_input) +
                        " expected_output=" + shapeString(expected_output);
                return false;
            }

            ov::AnyMap properties{
                {ov::hint::performance_mode.name(), ov::hint::PerformanceMode::LATENCY},
                {ov::inference_num_threads.name(), static_cast<int>(config.threads)},
            };
            compiled_model_ = core_.compile_model(model_, "CPU", properties);
            infer_request_ = compiled_model_.create_infer_request();
            input_ = infer_request_.get_input_tensor();
            output_ = infer_request_.get_output_tensor();
            input_size_ = input_.get_size();
            output_rows_ = config.output_rows;
            output_fields_ = config.output_fields;
            return true;
        } catch (const std::exception &exception) {
            error = exception.what();
            return false;
        }
    }

    MutableInferenceTensor inputTensor() override {
        return {input_.data<float>(), input_size_};
    }

    bool run(std::string &error) override {
        try {
            infer_request_.infer();
            return true;
        } catch (const std::exception &exception) {
            error = exception.what();
            return false;
        }
    }

    InferenceTensor outputTensor() override {
        return {output_.data<float>(), output_rows_, output_fields_};
    }

private:
    ov::Core core_;
    std::shared_ptr<ov::Model> model_;
    ov::CompiledModel compiled_model_;
    ov::InferRequest infer_request_;
    ov::Tensor input_;
    ov::Tensor output_;
    std::size_t input_size_ = 0;
    std::size_t output_rows_ = 0;
    std::size_t output_fields_ = 0;
};

}  // namespace

std::unique_ptr<InferenceBackend> createOpenVinoBackend() {
    return std::make_unique<OpenVinoBackend>();
}

}  // namespace eggvision
