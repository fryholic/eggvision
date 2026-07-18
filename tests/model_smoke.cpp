#include <openvino/openvino.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>

namespace {

long rssKb() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            long value = 0;
            status >> value;
            return value;
        }
        std::string rest;
        std::getline(status, rest);
    }
    return -1;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string model_path = argc > 1 ? argv[1] : "models/yolov5n.xml";
    const int iterations = argc > 2 ? std::stoi(argv[2]) : 100;
    const int threads = argc > 3 ? std::stoi(argv[3]) : 0;

    try {
        ov::Core core;
        auto model = core.read_model(model_path);
        ov::AnyMap properties{{ov::hint::performance_mode.name(), ov::hint::PerformanceMode::LATENCY}};
        if (threads > 0) {
            properties[ov::inference_num_threads.name()] = threads;
        }
        auto compiled = core.compile_model(model, "CPU", properties);
        auto request = compiled.create_infer_request();
        auto input = request.get_input_tensor();
        std::fill_n(input.data<float>(), input.get_size(), 0.0F);

        const long initial_rss = rssKb();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 1; i <= iterations; ++i) {
            request.infer();
            if (i == 1 || i % 100 == 0 || i == iterations) {
                std::cout << "iteration=" << i << " rss_kb=" << rssKb() << '\n';
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "model_smoke iterations=" << iterations
                  << " threads=" << threads
                  << " fps=" << iterations / seconds
                  << " initial_rss_kb=" << initial_rss
                  << " final_rss_kb=" << rssKb() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "model_smoke failed: " << error.what() << '\n';
        return 1;
    }
}
