#pragma once

#include "bsaps/config.hpp"
#include "bsaps/frame.hpp"
#include "bsaps/latest_frame_queue.hpp"
#include "bsaps/metrics.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <openvino/openvino.hpp>

namespace bsaps {

struct LetterboxTransform {
    float scale = 1.0F;
    int pad_x = 0;
    int pad_y = 0;
    int resized_width = 0;
    int resized_height = 0;
};

struct Detection {
    int class_id = 0;
    float confidence = 0.0F;
    cv::Rect2f box;
};

LetterboxTransform calculateLetterbox(int source_width,
                                      int source_height,
                                      int target_width,
                                      int target_height);
cv::Rect2f restoreLetterboxBox(const cv::Rect2f &box,
                               const LetterboxTransform &transform,
                               int source_width,
                               int source_height);
float intersectionOverUnion(const cv::Rect2f &a, const cv::Rect2f &b);
std::vector<Detection> nonMaximumSuppression(std::vector<Detection> detections,
                                             float iou_threshold);
void bgrToNormalizedRgbChw(const cv::Mat &bgr, float *destination);

class InferenceWorker {
public:
    InferenceWorker(const AppConfig &config, Metrics &metrics);
    ~InferenceWorker();

    InferenceWorker(const InferenceWorker &) = delete;
    InferenceWorker &operator=(const InferenceWorker &) = delete;

    bool initialize();
    bool start();
    void submit(std::shared_ptr<FrameLease> frame);
    void stop();

private:
    bool copyLoresI420(const StreamView &view, std::vector<std::uint8_t> &destination) const;
    std::vector<Detection> infer(const cv::Mat &bgr,
                                 double &preprocess_ms,
                                 double &inference_ms,
                                 double &postprocess_ms);
    void workerLoop();

    AppConfig config_;
    Metrics &metrics_;
    ov::Core core_;
    std::shared_ptr<ov::Model> model_;
    ov::CompiledModel compiled_model_;
    ov::InferRequest infer_request_;
    LatestFrameQueue<std::shared_ptr<FrameLease>> latest_;
    std::thread worker_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
};

}  // namespace bsaps
