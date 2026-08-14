#pragma once

#include "eggvision/config.hpp"
#include "eggvision/encoded_access_unit.hpp"
#include "eggvision/latest_frame_queue.hpp"
#include "eggvision/metrics.hpp"

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

namespace eggvision {

class RtspServer {
public:
    RtspServer(const AppConfig &config, Metrics &metrics);
    ~RtspServer();

    RtspServer(const RtspServer &) = delete;
    RtspServer &operator=(const RtspServer &) = delete;

    bool start();
    void submit(EncodedAccessUnitPtr unit);
    // Synchronous lifetime boundary: returns only after recovery workers and
    // every GStreamer object owned by this server have released their leases.
    // A stalled teardown is reported periodically and keeps blocking safely.
    void stop();
    std::string url(const std::string &host) const;
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    bool recoveryRunningForTest() const;
    bool runningForTest() const { return running_.load(std::memory_order_acquire); }
#endif

private:
    struct CallbackState;
    struct RecoveryJob;
    struct RecoveryWorkerState;
    struct SessionCleanupState;
    enum class RecoveryState {
        Idle,
        Requested,
        Running,
        Completed,
    };
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
    static void clientConnected(GstRTSPServer *server,
                                GstRTSPClient *client,
                                gpointer user_data);
    static void clientNewSession(GstRTSPClient *client,
                                 GstRTSPSession *session,
                                 gpointer user_data);
    static gboolean cleanupSessions(GstRTSPSessionPool *pool, gpointer user_data);
    static GstRTSPFilterResult closeClient(GstRTSPServer *, GstRTSPClient *client, gpointer);
    static GstRTSPFilterResult removeSession(GstRTSPSessionPool *, GstRTSPSession *, gpointer);
    GSource *createListenerSource();
    GSource *createWatchdogSource();
    static void destroySource(GSource *source);
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
    void onClientConnected(GstRTSPClient *client);
    void onClientNewSession(GstRTSPSession *session);
    bool startSessionCleanup();
    void requestSessionCleanupStop();
    bool startRecoveryWorker();
    void requestRecoveryWorkerStop();
    static void recoveryWorkerLoop(const std::shared_ptr<RecoveryWorkerState> &state);
    bool recoverMedia(std::uint64_t expected_media_generation, const char *reason) noexcept;
    void finishRecoveryIfReady();
    void disableCallbacksAndWait();
    void stopLocked();
    void feederLoop();
    GstBuffer *makeBuffer(EncodedAccessUnitPtr unit, std::uint64_t frame_index) const;

    AppConfig config_;
    Metrics &metrics_;
    GMainLoop *loop_ = nullptr;
    GstRTSPServer *server_ = nullptr;
    GstRTSPMountPoints *mounts_ = nullptr;
    GstRTSPMediaFactory *current_factory_ = nullptr;
    gulong current_factory_handler_ = 0;
    GSource *listener_source_ = nullptr;
    GSource *watchdog_source_ = nullptr;
    gulong client_connected_handler_ = 0;
    bool accepting_clients_ = false;
    std::thread loop_thread_;
    std::thread feeder_thread_;
    std::thread recovery_worker_thread_;
    std::shared_ptr<RecoveryWorkerState> recovery_worker_state_;
    std::shared_ptr<SessionCleanupState> session_cleanup_state_;
    LatestFrameQueue<EncodedAccessUnitPtr> latest_;
    mutable std::mutex source_mutex_;
    GstAppSrc *appsrc_ = nullptr;
    GstRTSPMedia *current_media_ = nullptr;
    MediaHandlers current_media_handlers_;
    std::shared_ptr<CallbackState> callback_state_;
    // Serializes complete start/stop epochs. source_mutex_ remains the shorter
    // callback/media lock and must never be used as the public lifecycle lock.
    mutable std::mutex lifecycle_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> generation_{0};
    // The following lifecycle fields are protected by source_mutex_. Keeping
    // status and recovery intent in the same media generation prevents an old
    // pipeline callback from retiring a newly configured pipeline.
    std::uint64_t media_generation_ = 0;
    unsigned consecutive_push_failures_ = 0;
    RecoveryState recovery_state_ = RecoveryState::Idle;
    std::uint64_t recovery_generation_ = 0;
    std::uint64_t recovery_token_ = 0;
    std::uint64_t next_recovery_token_ = 0;
    std::string recovery_reason_;
    gint64 recovery_started_us_ = 0;
    bool recovery_failure_reported_ = false;
    bool recovery_media_unprepared_ = false;
    std::shared_ptr<RecoveryJob> recovery_job_;
    GstRTSPMediaStatus observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
    gint64 status_since_us_ = 0;
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    std::string test_push_error_trigger_;
    unsigned test_push_errors_remaining_ = 0;
    bool test_push_error_consumed_ = false;
    unsigned test_teardown_delay_ms_ = 0;
    unsigned test_watchdog_recovery_delay_ms_ = 0;
    std::string test_recovery_pause_path_;
    bool test_recovery_pause_reported_ = false;
    unsigned test_session_timeout_seconds_ = 0;
    unsigned test_session_cleanup_delay_ms_ = 0;
    bool test_fail_recovery_thread_create_ = false;
    bool test_fail_cleanup_thread_create_ = false;
    bool test_fail_feeder_thread_create_ = false;
    bool test_fail_loop_thread_create_ = false;
#endif
};

}  // namespace eggvision
