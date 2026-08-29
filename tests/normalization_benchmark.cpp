#include "eggvision/inference.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Converter = void (*)(const cv::Mat &, float *);

#if defined(__GNUC__) || defined(__clang__)
#define EGGVISION_NOINLINE __attribute__((noinline))
#else
#define EGGVISION_NOINLINE
#endif

volatile float benchmark_sink = 0.0F;

EGGVISION_NOINLINE void divideReference(const cv::Mat &bgr, float *destination) {
    if (bgr.type() != CV_8UC3 || !destination) {
        throw std::invalid_argument("divideReference expects CV_8UC3 data");
    }
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

int parsePositive(const char *text, const char *name) {
    const long value = std::strtol(text, nullptr, 10);
    if (value <= 0 || value > 100000000L) {
        throw std::invalid_argument(std::string(name) + " must be positive");
    }
    return static_cast<int>(value);
}

double runBenchmark(const cv::Mat &input,
                    std::vector<float> &output,
                    Converter converter,
                    int iterations) {
    const auto start = Clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        converter(input, output.data());
    }
    const auto end = Clock::now();
    benchmark_sink = benchmark_sink + output[0] + output[output.size() / 2] + output.back();
    return std::chrono::duration<double, std::nano>(end - start).count() / iterations;
}

void warmUp(const cv::Mat &input,
            std::vector<float> &output,
            Converter converter,
            int iterations) {
    for (int iteration = 0; iteration < iterations; ++iteration) {
        converter(input, output.data());
    }
    benchmark_sink = benchmark_sink + output[0];
}

void printResult(int round, const char *variant, double nanoseconds_per_call) {
    std::cout << std::fixed << std::setprecision(3)
              << "{\"round\":" << round << ",\"variant\":\"" << variant
              << "\",\"ns_per_call\":" << nanoseconds_per_call << "}\n";
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const int iterations = argc > 1 ? parsePositive(argv[1], "iterations") : 10000;
        const int rounds = argc > 2 ? parsePositive(argv[2], "rounds") : 7;
        const int warmup_iterations = argc > 3 ? parsePositive(argv[3], "warmup iterations") : 250;

        cv::Mat input(320, 320, CV_8UC3);
        std::mt19937 random(0x45564732U);
        for (int y = 0; y < input.rows; ++y) {
            auto *row = input.ptr<cv::Vec3b>(y);
            for (int x = 0; x < input.cols; ++x) {
                row[x] = cv::Vec3b(static_cast<std::uint8_t>(random() & 0xffU),
                                   static_cast<std::uint8_t>(random() & 0xffU),
                                   static_cast<std::uint8_t>(random() & 0xffU));
            }
        }

        const std::size_t output_size = input.total() * 3;
        std::vector<float> divide_output(output_size);
        std::vector<float> production_output(output_size);
        warmUp(input, divide_output, divideReference, warmup_iterations);
        warmUp(input,
               production_output,
               eggvision::bgrToNormalizedRgbChw,
               warmup_iterations);

        std::cout << "{\"type\":\"normalization_benchmark\",\"width\":" << input.cols
                  << ",\"height\":" << input.rows << ",\"iterations\":" << iterations
                  << ",\"rounds\":" << rounds
                  << ",\"warmup_iterations\":" << warmup_iterations << "}\n";
        for (int round = 1; round <= rounds; ++round) {
            if ((round & 1) != 0) {
                printResult(round,
                            "divide",
                            runBenchmark(input, divide_output, divideReference, iterations));
                printResult(round,
                            "production",
                            runBenchmark(input,
                                         production_output,
                                         eggvision::bgrToNormalizedRgbChw,
                                         iterations));
            } else {
                printResult(round,
                            "production",
                            runBenchmark(input,
                                         production_output,
                                         eggvision::bgrToNormalizedRgbChw,
                                         iterations));
                printResult(round,
                            "divide",
                            runBenchmark(input, divide_output, divideReference, iterations));
            }
        }
        std::cout << std::setprecision(9) << "{\"sink\":" << benchmark_sink << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "normalization benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
