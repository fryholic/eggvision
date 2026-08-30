#include "eggvision/inference_backend.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace eggvision {

std::unique_ptr<InferenceBackend> createOpenVinoBackend();
std::unique_ptr<InferenceBackend> createMnnBackend();

std::unique_ptr<InferenceBackend> createInferenceBackend(std::string_view name) {
    if (name == "openvino") {
        return createOpenVinoBackend();
    }
    if (name == "mnn") {
        return createMnnBackend();
    }
    throw std::invalid_argument("unsupported inference backend: " + std::string(name));
}

}  // namespace eggvision
