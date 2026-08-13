#pragma once

#include "bsaps/config.hpp"
#include "bsaps/frame.hpp"
#include "bsaps/latest_frame_queue.hpp"
#include "bsaps/metrics.hpp"

#include <atomic>
#include <condition_variable>
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
    struct CallbackState;
    struct MediaHandlers {
        gulong prepared = 0;
        gulong unprepared = 0;
        gulong target_state = 0;
        gulong new_state = 0;
        gulong handle_message = 0;
    };
    class CallbackGuard {
    public:
        explicit CallbackGuard(gpointer user_data);
        ~CallbackGuard();

        RtspServer *server() const { return server_; }

    private:
        std::shared_ptr<CallbackState> state_;
        RtspServer *server_ = nullptr;
    };

    static gpointer newCallbackData(const std::shared_ptr<CallbackState> &state);
    static void destroySignalCallbackData(gpointer data, GClosure *);
    static void destroySourceCallbackData(gpointer data);
    static void mediaConfigure(GstRTSPMediaFactory *factory,
                               GstRTSPMedia *media,
                               gpointer user_data);
    static void mediaPrepared(GstRTSPMedia *media, gpointer user_data);
    static void mediaUnprepared(GstRTSPMedia *media, gpointer user_data);
    static void mediaTargetState(GstRTSPMedia *media, GstState state, gpointer user_data);
    static void mediaNewState(GstRTSPMedia *media, GstState state, gpointer user_data);
    static gboolean mediaHandleMessage(GstRTSPMedia *media,
                                       GstMessage *message,
                                       gpointer user_data);
    static gboolean watchdogTick(gpointer user_data);
    static GstRTSPFilterResult closeClient(GstRTSPServer *, GstRTSPClient *client, gpointer);
    bool installFactory();
    bool bindMediaSource(GstRTSPMedia *media);
    MediaHandlers connectMediaHandlers(GstRTSPMedia *media);
    static void disconnectMediaHandlers(GstRTSPMedia *media, const MediaHandlers &handlers);
    void onMediaConfigure(GstRTSPMediaFactory *factory, GstRTSPMedia *media);
    void onMediaPrepared(GstRTSPMedia *media);
    void onMediaUnprepared(GstRTSPMedia *media);
    void onMediaTargetState(GstRTSPMedia *media, GstState state);
    void onMediaNewState(GstRTSPMedia *media, GstState state);
    gboolean onMediaHandleMessage(GstRTSPMedia *media, GstMessage *message);
    gboolean onWatchdog();
    bool recoverMedia(std::uint64_t expected_media_generation, const char *reason);
    void disableCallbacksAndWait();
    void feederLoop();
    GstBuffer *makeBuffer(std::shared_ptr<FrameLease> frame,
                          std::uint64_t base_timestamp,
                          std::uint64_t frame_index) const;

    AppConfig config_;
    Metrics &metrics_;
    GMainLoop *loop_ = nullptr;
    GstRTSPServer *server_ = nullptr;
    GstRTSPMountPoints *mounts_ = nullptr;
    GstRTSPMediaFactory *current_factory_ = nullptr;
    gulong current_factory_handler_ = 0;
    guint attach_id_ = 0;
    guint watchdog_id_ = 0;
    std::thread loop_thread_;
    std::thread feeder_thread_;
    LatestFrameQueue<std::shared_ptr<FrameLease>> latest_;
    mutable std::mutex source_mutex_;
    GstAppSrc *appsrc_ = nullptr;
    GstRTSPMedia *current_media_ = nullptr;
    MediaHandlers current_media_handlers_;
    std::shared_ptr<CallbackState> callback_state_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> generation_{0};
    // The following lifecycle fields are protected by source_mutex_. Keeping
    // status and recovery intent in the same media generation prevents an old
    // pipeline callback from retiring a newly configured pipeline.
    std::uint64_t media_generation_ = 0;
    unsigned consecutive_push_failures_ = 0;
    std::uint64_t recovery_generation_ = 0;
    std::string recovery_reason_;
    GstRTSPMediaStatus observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
    gint64 status_since_us_ = 0;
    gint64 last_session_cleanup_us_ = 0;
};

}  // namespace bsaps
