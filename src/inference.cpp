#include "eggvision/inference.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

namespace eggvision {
namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

LetterboxTransform calculateLetterbox(int source_width,
                                      int source_height,
                                      int target_width,
                                      int target_height) {
    LetterboxTransform transform;
    transform.scale = std::min(static_cast<float>(target_width) / source_width,
                               static_cast<float>(target_height) / source_height);
    transform.resized_width = static_cast<int>(std::round(source_width * transform.scale));
    transform.resized_height = static_cast<int>(std::round(source_height * transform.scale));
    transform.pad_x = (target_width - transform.resized_width) / 2;
    transform.pad_y = (target_height - transform.resized_height) / 2;
    return transform;
}

cv::Rect2f restoreLetterboxBox(const cv::Rect2f &box,
                               const LetterboxTransform &transform,
                               int source_width,
                               int source_height) {
    const float left = std::clamp((box.x - transform.pad_x) / transform.scale,
                                  0.0F,
                                  static_cast<float>(source_width));
    const float top = std::clamp((box.y - transform.pad_y) / transform.scale,
                                 0.0F,
                                 static_cast<float>(source_height));
    const float right = std::clamp((box.x + box.width - transform.pad_x) / transform.scale,
                                   0.0F,
                                   static_cast<float>(source_width));
    const float bottom = std::clamp((box.y + box.height - transform.pad_y) / transform.scale,
                                    0.0F,
                                    static_cast<float>(source_height));
    return {left, top, std::max(0.0F, right - left), std::max(0.0F, bottom - top)};
}

float intersectionOverUnion(const cv::Rect2f &a, const cv::Rect2f &b) {
    const float intersection = (a & b).area();
    const float union_area = a.area() + b.area() - intersection;
    return union_area > 0.0F ? intersection / union_area : 0.0F;
}

std::vector<Detection> nonMaximumSuppression(std::vector<Detection> detections,
                                             float iou_threshold) {
    std::sort(detections.begin(), detections.end(), [](const Detection &left, const Detection &right) {
        return left.confidence > right.confidence;
    });
    std::vector<Detection> result;
    std::vector<bool> suppressed(detections.size(), false);
    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (suppressed[i]) {
            continue;
        }
        result.push_back(detections[i]);
        for (std::size_t j = i + 1; j < detections.size(); ++j) {
            if (!suppressed[j] && detections[i].class_id == detections[j].class_id &&
                intersectionOverUnion(detections[i].box, detections[j].box) > iou_threshold) {
                suppressed[j] = true;
            }
        }
    }
    return result;
}

void bgrToNormalizedRgbChw(const cv::Mat &bgr, float *destination) {
    if (bgr.type() != CV_8UC3 || !destination) {
        throw std::invalid_argument("bgrToNormalizedRgbChw expects CV_8UC3 data");
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

InferenceWorker::InferenceWorker(const AppConfig &config, Metrics &metrics)
    : config_(config), metrics_(metrics) {}

InferenceWorker::~InferenceWorker() {
    stop();
}

bool InferenceWorker::initialize() {
    if (!config_.inference_enabled) {
        initialized_.store(true);
        return true;
    }
    try {
        if (!std::filesystem::exists(config_.model_path)) {
            std::cerr << "[inference] model not found: " << config_.model_path << '\n';
            return false;
        }
        model_ = core_.read_model(config_.model_path);
        const auto input_shape = model_->input().get_shape();
        const auto output_shape = model_->output().get_shape();
        const ov::Shape expected_input{1, 3, config_.inference_height, config_.inference_width};
        if (input_shape != expected_input || output_shape.size() != 3 || output_shape[0] != 1 ||
            output_shape[2] < 6) {
            std::cerr << "[inference] unexpected model shapes input=" << input_shape
                      << " output=" << output_shape << '\n';
            return false;
        }
        ov::AnyMap properties{
            {ov::hint::performance_mode.name(), ov::hint::PerformanceMode::LATENCY},
            {ov::inference_num_threads.name(), static_cast<int>(config_.inference_threads)},
        };
        compiled_model_ = core_.compile_model(model_, "CPU", properties);
        infer_request_ = compiled_model_.create_infer_request();
        initialized_.store(true);
        std::cout << "[inference] OpenVINO model=" << config_.model_path
                  << " input=" << input_shape << " output=" << output_shape
                  << " device=CPU threads=" << config_.inference_threads << '\n';
        return true;
    } catch (const std::exception &error) {
        std::cerr << "[inference] initialization failed: " << error.what() << '\n';
        return false;
    }
}

bool InferenceWorker::start() {
    if (!initialized_.load()) {
        return false;
    }
    if (!config_.inference_enabled) {
        std::cout << "[inference] disabled by configuration\n";
        return true;
    }
    if (running_.exchange(true)) {
        return true;
    }
    worker_ = std::thread(&InferenceWorker::workerLoop, this);
    return true;
}

void InferenceWorker::setDetectionConsumer(DetectionConsumer consumer) {
    detection_consumer_ = std::move(consumer);
}

void InferenceWorker::submit(std::shared_ptr<FrameLease> frame) {
    if (!running_.load()) {
        return;
    }
    if (latest_.push(std::move(frame))) {
        metrics_.inference_dropped.fetch_add(1);
    }
}

bool InferenceWorker::copyLoresI420(const StreamView &view,
                                    std::vector<std::uint8_t> &destination) const {
    if (view.width == 0 || view.height == 0 || view.stride < view.width || view.planes.empty()) {
        return false;
    }
    const std::size_t y_size = static_cast<std::size_t>(view.width) * view.height;
    const std::size_t chroma_size = y_size / 4;
    destination.resize(y_size + 2 * chroma_size);

    auto copy_rows = [](std::uint8_t *target,
                        const std::uint8_t *source,
                        unsigned rows,
                        unsigned width,
                        unsigned stride) {
        for (unsigned row = 0; row < rows; ++row) {
            std::copy_n(source + static_cast<std::size_t>(row) * stride,
                        width,
                        target + static_cast<std::size_t>(row) * width);
        }
    };

    if (view.planes.size() == 1 && view.planes[0].data) {
        const std::uint8_t *base = view.planes[0].data;
        const unsigned chroma_stride = view.stride / 2;
        const std::uint8_t *u = base + static_cast<std::size_t>(view.stride) * view.height;
        const std::uint8_t *v = u + static_cast<std::size_t>(chroma_stride) * (view.height / 2);
        copy_rows(destination.data(), base, view.height, view.width, view.stride);
        copy_rows(destination.data() + y_size,
                  u,
                  view.height / 2,
                  view.width / 2,
                  chroma_stride);
        copy_rows(destination.data() + y_size + chroma_size,
                  v,
                  view.height / 2,
                  view.width / 2,
                  chroma_stride);
        return true;
    }
    if (view.planes.size() == 3 && view.planes[0].data && view.planes[1].data &&
        view.planes[2].data) {
        copy_rows(destination.data(), view.planes[0].data, view.height, view.width, view.stride);
        copy_rows(destination.data() + y_size,
                  view.planes[1].data,
                  view.height / 2,
                  view.width / 2,
                  view.stride / 2);
        copy_rows(destination.data() + y_size + chroma_size,
                  view.planes[2].data,
                  view.height / 2,
                  view.width / 2,
                  view.stride / 2);
        return true;
    }
    return false;
}

std::vector<Detection> InferenceWorker::infer(const cv::Mat &bgr,
                                              double &preprocess_ms,
                                              double &inference_ms,
                                              double &postprocess_ms) {
    const auto preprocess_start = Clock::now();
    const auto transform = calculateLetterbox(bgr.cols,
                                              bgr.rows,
                                              config_.inference_width,
                                              config_.inference_height);
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(transform.resized_width, transform.resized_height));
    cv::Mat letterboxed(config_.inference_height,
                        config_.inference_width,
                        CV_8UC3,
                        cv::Scalar(114, 114, 114));
    resized.copyTo(letterboxed(cv::Rect(transform.pad_x,
                                        transform.pad_y,
                                        transform.resized_width,
                                        transform.resized_height)));

    ov::Tensor input = infer_request_.get_input_tensor();
    float *tensor = input.data<float>();
    bgrToNormalizedRgbChw(letterboxed, tensor);
    const auto preprocess_end = Clock::now();

    infer_request_.infer();
    const auto inference_end = Clock::now();

    ov::Tensor output = infer_request_.get_output_tensor();
    const auto shape = output.get_shape();
    const float *data = output.data<float>();
    const std::size_t fields = shape[2];
    std::vector<Detection> detections;
    for (std::size_t i = 0; i < shape[1]; ++i) {
        const float *row = data + i * fields;
        const float confidence = row[4] * row[5];  // COCO class 0: person.
        if (confidence < config_.confidence_threshold) {
            continue;
        }
        const cv::Rect2f model_box(row[0] - row[2] / 2.0F,
                                   row[1] - row[3] / 2.0F,
                                   row[2],
                                   row[3]);
        cv::Rect2f restored = restoreLetterboxBox(model_box, transform, bgr.cols, bgr.rows);
        if (restored.width > 0.0F && restored.height > 0.0F) {
            detections.push_back({0, confidence, restored});
        }
    }
    detections = nonMaximumSuppression(std::move(detections), config_.nms_threshold);
    const auto postprocess_end = Clock::now();

    preprocess_ms = milliseconds(preprocess_start, preprocess_end);
    inference_ms = milliseconds(preprocess_end, inference_end);
    postprocess_ms = milliseconds(inference_end, postprocess_end);
    return detections;
}

void InferenceWorker::workerLoop() {
    std::vector<std::uint8_t> i420;
    while (running_.load()) {
        std::shared_ptr<FrameLease> frame;
        if (!latest_.waitPop(frame)) {
            break;
        }
        const StreamView &view = frame->lores();
        if (!copyLoresI420(view, i420)) {
            std::cerr << "[inference] unsupported lores plane layout\n";
            continue;
        }
        cv::Mat yuv(static_cast<int>(view.height * 3 / 2),
                    static_cast<int>(view.width),
                    CV_8UC1,
                    i420.data());
        cv::Mat bgr;
        cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);

        try {
            double preprocess_ms = 0.0;
            double inference_ms = 0.0;
            double postprocess_ms = 0.0;
            auto detections = infer(bgr, preprocess_ms, inference_ms, postprocess_ms);
            const double total_ms = preprocess_ms + inference_ms + postprocess_ms;
            const std::uint64_t processed = metrics_.inference_processed.fetch_add(1) + 1;
            metrics_.detected_persons.fetch_add(detections.size());
            metrics_.inference_total_us.fetch_add(static_cast<std::uint64_t>(total_ms * 1000.0));
            if (!detections.empty() && detection_consumer_) {
                detection_consumer_(frame, detections);
            }
            if (processed % 10 == 0 || !detections.empty()) {
                std::cout << std::fixed << std::setprecision(2)
                          << "{\"type\":\"inference\",\"sequence\":" << frame->sequence()
                          << ",\"persons\":" << detections.size()
                          << ",\"preprocess_ms\":" << preprocess_ms
                          << ",\"inference_ms\":" << inference_ms
                          << ",\"postprocess_ms\":" << postprocess_ms
                          << ",\"total_ms\":" << total_ms << "}\n";
            }
        } catch (const std::exception &error) {
            std::cerr << "[inference] frame failed: " << error.what() << '\n';
        }
    }
}

void InferenceWorker::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    latest_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::cout << "[inference] stopped\n";
}

}  // namespace eggvision
