#include "eggvision/inference.hpp"
#include "eggvision/logging.hpp"
#include "eggvision/dma_buf_sync.hpp"
#include "eggvision/i420.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>

#include <opencv2/imgproc.hpp>
#include <glib.h>

namespace eggvision {
namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void recordI420Rejection(Metrics &metrics, CompactI420Status status) {
    switch (status) {
        case CompactI420Status::Compatible:
            return;
        case CompactI420Status::InvalidDimensions:
            metrics.inference_i420_invalid_dimensions.fetch_add(1);
            return;
        case CompactI420Status::UnexpectedPlaneCount:
            metrics.inference_i420_unexpected_plane_count.fetch_add(1);
            return;
        case CompactI420Status::UnexpectedStride:
            metrics.inference_i420_unexpected_stride.fetch_add(1);
            return;
        case CompactI420Status::InvalidFileDescriptor:
            metrics.inference_i420_invalid_fd.fetch_add(1);
            return;
        case CompactI420Status::DifferentFileDescriptors:
            metrics.inference_i420_separate_fds.fetch_add(1);
            return;
        case CompactI420Status::MissingMapping:
            metrics.inference_i420_missing_mapping.fetch_add(1);
            return;
        case CompactI420Status::InconsistentMapping:
            metrics.inference_i420_inconsistent_mapping.fetch_add(1);
            return;
        case CompactI420Status::NonCompactOffsets:
            metrics.inference_i420_non_compact_offsets.fetch_add(1);
            return;
        case CompactI420Status::PlaneTooShort:
            metrics.inference_i420_plane_too_short.fetch_add(1);
            return;
        case CompactI420Status::PayloadTooShort:
            metrics.inference_i420_payload_too_short.fetch_add(1);
            return;
        case CompactI420Status::MappingTooShort:
            metrics.inference_i420_mapping_too_short.fetch_add(1);
            return;
    }
}

std::string sha256File(const std::string &path) {
    struct ChecksumDeleter {
        void operator()(GChecksum *checksum) const noexcept {
            g_checksum_free(checksum);
        }
    };
    std::unique_ptr<GChecksum, ChecksumDeleter> checksum(
        g_checksum_new(G_CHECKSUM_SHA256));
    if (!checksum) {
        throw std::runtime_error("cannot create SHA-256 checksum");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open model for SHA-256: " + path);
    }
    char buffer[64 * 1024];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            g_checksum_update(checksum.get(),
                              reinterpret_cast<const guchar *>(buffer),
                              static_cast<gsize>(count));
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("cannot read model for SHA-256: " + path);
    }
    return g_checksum_get_string(checksum.get());
}

}  // namespace

std::string inferenceModelFingerprint(const std::string &backend,
                                      const std::string &model_path) {
    const std::string model_hash = sha256File(model_path);
    if (backend == "mnn") {
        return model_hash;
    }
    if (backend == "openvino") {
        std::filesystem::path weights_path(model_path);
        weights_path.replace_extension(".bin");
        return "xml:" + model_hash + ",bin:" + sha256File(weights_path.string());
    }
    throw std::invalid_argument("unsupported inference backend: " + backend);
}

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
    constexpr float kInv255 = 1.0F / 255.0F;
    const std::size_t plane_size = static_cast<std::size_t>(bgr.cols) * bgr.rows;
    for (int y = 0; y < bgr.rows; ++y) {
        const auto *row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * bgr.cols + x;
            destination[index] = static_cast<float>(row[x][2]) * kInv255;
            destination[plane_size + index] = static_cast<float>(row[x][1]) * kInv255;
            destination[2 * plane_size + index] = static_cast<float>(row[x][0]) * kInv255;
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
            synchronizedLog(std::cerr) << "[inference] model not found: " << config_.model_path << '\n';
            return false;
        }
        model_sha256_ =
            inferenceModelFingerprint(config_.inference_backend, config_.model_path);
        backend_ = createInferenceBackend(config_.inference_backend);
        InferenceBackendConfig backend_config;
        backend_config.model_path = config_.model_path;
        backend_config.input_width = config_.inference_width;
        backend_config.input_height = config_.inference_height;
        backend_config.threads = config_.inference_threads;
        std::string error;
        if (!backend_->initialize(backend_config, error)) {
            synchronizedLog(std::cerr) << "[inference] " << backend_->name()
                      << " initialization failed: " << error << '\n';
            metrics_.inference_backend_errors.fetch_add(1);
            backend_.reset();
            return false;
        }
        initialized_.store(true);
        synchronizedLog(std::cout) << "[inference] backend=" << backend_->name()
                  << " model=" << config_.model_path
                  << " model_sha256=" << model_sha256_
                  << " input=[1,3," << config_.inference_height << ','
                  << config_.inference_width << "] output=[1,6300,85]"
                  << " device=CPU threads=" << config_.inference_threads << '\n';
        return true;
    } catch (const std::exception &error) {
        synchronizedLog(std::cerr) << "[inference] initialization failed: " << error.what() << '\n';
        return false;
    }
}

const std::string &InferenceWorker::modelSha256() const noexcept {
    return model_sha256_;
}

bool InferenceWorker::start() {
    if (!initialized_.load()) {
        return false;
    }
    if (!config_.inference_enabled) {
        synchronizedLog(std::cout) << "[inference] disabled by configuration\n";
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

    MutableInferenceTensor input = backend_->inputTensor();
    const std::size_t expected_input_size =
        static_cast<std::size_t>(3) * config_.inference_width * config_.inference_height;
    if (!input || input.size != expected_input_size) {
        throw std::runtime_error("inference backend returned an invalid input tensor");
    }
    bgrToNormalizedRgbChw(letterboxed, input.data);
    const auto preprocess_end = Clock::now();

    std::string inference_error;
    if (!backend_->run(inference_error)) {
        metrics_.inference_backend_errors.fetch_add(1);
        throw std::runtime_error("inference backend failed: " + inference_error);
    }
    const auto inference_end = Clock::now();

    const InferenceTensor output = backend_->outputTensor();
    if (!output || output.fields < 6) {
        throw std::runtime_error("inference backend returned an invalid output tensor");
    }
    std::vector<Detection> detections;
    for (std::size_t i = 0; i < output.rows; ++i) {
        const float *row = output.data + i * output.fields;
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
    cv::Mat bgr;
    std::optional<CompactI420Status> reported_rejection;
    while (running_.load()) {
        std::shared_ptr<FrameLease> frame;
        if (!latest_.waitPop(frame)) {
            break;
        }
        const StreamView &view = frame->lores();
        try {
            const auto input_start = Clock::now();
            const CompactI420View compact = inspectCompactI420(view);
            std::string sync_error;
            DmaBufReadSync read_sync(view, sync_error);
            if (!read_sync) {
                metrics_.inference_preprocess_errors.fetch_add(1);
                metrics_.inference_dma_sync_errors.fetch_add(1);
                synchronizedLog(std::cerr) << "[inference] DMA-BUF read sync failed: " << sync_error << '\n';
                continue;
            }

            const std::uint8_t *i420_data = nullptr;
            if (compact) {
                metrics_.inference_zero_copy_ingress.fetch_add(1);
                i420_data = compact.data;
            } else {
                metrics_.inference_copy_fallback.fetch_add(1);
                recordI420Rejection(metrics_, compact.status);
                if (!reported_rejection || *reported_rejection != compact.status) {
                    synchronizedLog(std::cerr) << "[inference] I420 copy fallback reason="
                              << compactI420StatusName(compact.status) << '\n';
                    reported_rejection = compact.status;
                }
                std::string error;
                if (!copyMappedI420(view, i420, error)) {
                    metrics_.inference_preprocess_errors.fetch_add(1);
                    synchronizedLog(std::cerr) << "[inference] I420 copy fallback failed: " << error << '\n';
                    continue;
                }
                i420_data = i420.data();
            }

            if (compact) {
                cv::Mat yuv(static_cast<int>(view.height * 3 / 2),
                            static_cast<int>(view.width),
                            CV_8UC1,
                            const_cast<std::uint8_t *>(i420_data));
                cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
                if (!read_sync.finish(sync_error)) {
                    metrics_.inference_preprocess_errors.fetch_add(1);
                    metrics_.inference_dma_sync_errors.fetch_add(1);
                    synchronizedLog(std::cerr) << "[inference] DMA-BUF read sync failed: " << sync_error << '\n';
                    continue;
                }
            } else {
                if (!read_sync.finish(sync_error)) {
                    metrics_.inference_preprocess_errors.fetch_add(1);
                    metrics_.inference_dma_sync_errors.fetch_add(1);
                    synchronizedLog(std::cerr) << "[inference] DMA-BUF read sync failed: " << sync_error << '\n';
                    continue;
                }
                cv::Mat yuv(static_cast<int>(view.height * 3 / 2),
                            static_cast<int>(view.width),
                            CV_8UC1,
                            i420.data());
                cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
            }
            const auto input_end = Clock::now();
            const double input_ms = milliseconds(input_start, input_end);

            double preprocess_ms = 0.0;
            double inference_ms = 0.0;
            double postprocess_ms = 0.0;
            auto detections = infer(bgr, preprocess_ms, inference_ms, postprocess_ms);
            const double total_ms = preprocess_ms + inference_ms + postprocess_ms;
            const double pipeline_ms = input_ms + total_ms;
            const std::uint64_t processed = metrics_.inference_processed.fetch_add(1) + 1;
            metrics_.detected_persons.fetch_add(detections.size());
            metrics_.inference_input_total_us.fetch_add(
                static_cast<std::uint64_t>(input_ms * 1000.0));
            metrics_.inference_total_us.fetch_add(static_cast<std::uint64_t>(total_ms * 1000.0));
            if (!detections.empty() && detection_consumer_) {
                detection_consumer_(frame, detections);
            }
            if (processed % 10 == 0 || !detections.empty()) {
                synchronizedLog(std::cout) << std::fixed << std::setprecision(2)
                          << "{\"type\":\"inference\",\"sequence\":" << frame->sequence()
                          << ",\"persons\":" << detections.size()
                          << ",\"input_ms\":" << input_ms
                          << ",\"preprocess_ms\":" << preprocess_ms
                          << ",\"inference_ms\":" << inference_ms
                          << ",\"postprocess_ms\":" << postprocess_ms
                          << ",\"total_ms\":" << total_ms
                          << ",\"pipeline_ms\":" << pipeline_ms << "}\n";
            }
        } catch (const std::exception &error) {
            synchronizedLog(std::cerr) << "[inference] frame failed: " << error.what() << '\n';
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
    synchronizedLog(std::cout) << "[inference] stopped\n";
}

}  // namespace eggvision
