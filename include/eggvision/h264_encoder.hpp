#pragma once

#include "eggvision/config.hpp"
#include "eggvision/encoded_access_unit.hpp"
#include "eggvision/frame.hpp"
#include "eggvision/latest_frame_queue.hpp"
#include "eggvision/metrics.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace eggvision {

// Owns the single always-on Raspberry Pi hardware H.264 encoder. Its lifetime
// is independent of RTSP clients, which makes encoded pre-roll available even
// before the first network connection.
class H264Encoder {
public:
    using Consumer = std::function<void(EncodedAccessUnitPtr)>;

    H264Encoder(const AppConfig &config, Metrics &metrics);
    ~H264Encoder();

    H264Encoder(const H264Encoder &) = delete;
    H264Encoder &operator=(const H264Encoder &) = delete;

    void setConsumer(Consumer consumer);
    bool initialize();
    bool start();
    void submit(std::shared_ptr<FrameLease> frame);
    void stop();
    bool waitForIndependentFrame(std::chrono::milliseconds timeout);
    bool recoveryRequested() const { return recovery_requested_.load(); }

private:
    void inputLoop();
    void outputLoop();
    void busLoop();
    void releasePipeline();
    GstBuffer *makeRawBuffer(std::shared_ptr<FrameLease> frame,
                             std::uint64_t base_sensor_timestamp_ns,
                             std::uint64_t frame_index) const;

    AppConfig config_;
    Metrics &metrics_;
    LatestFrameQueue<std::shared_ptr<FrameLease>> latest_;
    Consumer consumer_;
    std::mutex consumer_mutex_;
    std::mutex lifecycle_mutex_;
    GstElement *pipeline_ = nullptr;
    GstAppSrc *appsrc_ = nullptr;
    GstAppSink *appsink_ = nullptr;
    std::thread input_thread_;
    std::thread output_thread_;
    std::thread bus_thread_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> base_sensor_timestamp_ns_{0};
    std::atomic<bool> recovery_requested_{false};
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    std::atomic<bool> test_failure_injected_{false};
#endif
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    bool independent_frame_seen_ = false;
};

}  // namespace eggvision
