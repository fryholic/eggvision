#pragma once

#include "bsaps/config.hpp"
#include "bsaps/frame.hpp"
#include "bsaps/latest_frame_queue.hpp"
#include "bsaps/metrics.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

namespace bsaps {

class RtspServer {
public:
    RtspServer(const AppConfig &config, Metrics &metrics);
    ~RtspServer();

    RtspServer(const RtspServer &) = delete;
    RtspServer &operator=(const RtspServer &) = delete;

    bool start();
    void submit(std::shared_ptr<FrameLease> frame);
    void stop();
    std::string url(const std::string &host) const;

private:
    static void mediaConfigure(GstRTSPMediaFactory *, GstRTSPMedia *media, gpointer user_data);
    static void mediaUnprepared(GstRTSPMedia *media, gpointer user_data);
    void onMediaConfigure(GstRTSPMedia *media);
    void onMediaUnprepared();
    void feederLoop();
    GstBuffer *makeBuffer(std::shared_ptr<FrameLease> frame,
                          std::uint64_t base_timestamp,
                          std::uint64_t frame_index) const;

    AppConfig config_;
    Metrics &metrics_;
    GMainLoop *loop_ = nullptr;
    GstRTSPServer *server_ = nullptr;
    guint attach_id_ = 0;
    std::thread loop_thread_;
    std::thread feeder_thread_;
    LatestFrameQueue<std::shared_ptr<FrameLease>> latest_;
    mutable std::mutex source_mutex_;
    GstAppSrc *appsrc_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> generation_{0};
};

}  // namespace bsaps

