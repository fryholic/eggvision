#include "bsaps/rtsp_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <unistd.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

namespace bsaps {
namespace {

GQuark leaseQuark() {
    static const GQuark quark = g_quark_from_static_string("bsaps-frame-lease");
    return quark;
}

void destroyLease(gpointer data) {
    delete static_cast<std::shared_ptr<FrameLease> *>(data);
}

}  // namespace

struct RtspServer::CallbackState {
    std::mutex mutex;
    std::condition_variable cv;
    RtspServer *server = nullptr;
    unsigned active = 0;
};

RtspServer::CallbackGuard::CallbackGuard(gpointer user_data) {
    auto *holder = static_cast<std::shared_ptr<CallbackState> *>(user_data);
    if (!holder) {
        return;
    }
    state_ = *holder;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->server) {
        server_ = state_->server;
        ++state_->active;
    }
}

RtspServer::CallbackGuard::~CallbackGuard() {
    if (!server_) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (--state_->active == 0) {
        state_->cv.notify_all();
    }
}

gpointer RtspServer::newCallbackData(const std::shared_ptr<CallbackState> &state) {
    return new std::shared_ptr<CallbackState>(state);
}

void RtspServer::destroySignalCallbackData(gpointer data, GClosure *) {
    delete static_cast<std::shared_ptr<CallbackState> *>(data);
}

void RtspServer::destroySourceCallbackData(gpointer data) {
    delete static_cast<std::shared_ptr<CallbackState> *>(data);
}

RtspServer::RtspServer(const AppConfig &config, Metrics &metrics)
    : config_(config), metrics_(metrics), callback_state_(std::make_shared<CallbackState>()) {
    callback_state_->server = this;
#ifdef BSAPS_ENABLE_TEST_HOOKS
    // This hook is compiled out of normal builds. Test builds opt in through
    // BSAPS_ENABLE_TEST_HOOKS and still remain inert unless the trigger path is
    // explicitly supplied in the environment.
    if (const gchar *trigger = g_getenv("BSAPS_RTSP_TEST_PUSH_ERROR_TRIGGER")) {
        test_push_error_trigger_ = trigger;
    }
#endif
    gst_init(nullptr, nullptr);
}

RtspServer::~RtspServer() {
    stop();
}

bool RtspServer::installFactory() {
    if (!running_.load() || !mounts_) {
        return false;
    }

    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
    std::ostringstream pipeline;
    pipeline << "( appsrc name=source is-live=true format=time do-timestamp=false block=false "
             << "max-buffers=1 leaky-type=downstream "
             // GStreamer 1.22's v4l2 encoder imports GstDmaBufMemory but its pad
             // template advertises plain video/x-raw. Adding the memory feature
             // causes a not-linked error; the memory object remains DMABUF here.
             << "! video/x-raw,format=I420,width=" << config_.main_width
             << ",height=" << config_.main_height << ",framerate=" << config_.fps
             << "/1,colorimetry=bt709,interlace-mode=progressive,pixel-aspect-ratio=1/1 "
             << "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
             << "! v4l2h264enc output-io-mode=5 capture-io-mode=2 "
             << "extra-controls=\"controls,h264_level=11,h264_profile=4,video_bitrate="
             << config_.bitrate << ",video_gop_size=" << config_.gop
             << ",h264_i_frame_period=" << config_.gop
             << ",repeat_sequence_header=1\" "
             << "! video/x-h264,profile=high,level=(string)4 "
             << "! h264parse config-interval=1 "
             << "! rtph264pay name=pay0 pt=96 config-interval=1 mtu=1400 )";

    gst_rtsp_media_factory_set_launch(factory, pipeline.str().c_str());
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_media_factory_set_stop_on_disconnect(factory, FALSE);
    gst_rtsp_media_factory_set_eos_shutdown(factory, FALSE);
    gst_rtsp_media_factory_set_suspend_mode(factory, GST_RTSP_SUSPEND_MODE_NONE);
    gst_rtsp_media_factory_set_protocols(
        factory, static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));
    gst_rtsp_media_factory_set_latency(factory, 50);
    const gulong handler = g_signal_connect_data(
        factory,
        "media-configure",
        G_CALLBACK(RtspServer::mediaConfigure),
        newCallbackData(callback_state_),
        RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));

    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (!running_.load() || current_factory_ != nullptr) {
            g_signal_handler_disconnect(factory, handler);
            g_object_unref(factory);
            return false;
        }
        current_factory_ = static_cast<GstRTSPMediaFactory *>(g_object_ref(factory));
        current_factory_handler_ = handler;
    }
    gst_rtsp_mount_points_add_factory(mounts_, config_.rtsp_mount.c_str(), factory);
    return true;
}

bool RtspServer::start() {
    if (running_.exchange(true)) {
        return true;
    }

    loop_ = g_main_loop_new(nullptr, FALSE);
    server_ = gst_rtsp_server_new();
    gst_rtsp_server_set_address(server_, config_.rtsp_address.c_str());
    gst_rtsp_server_set_service(server_, config_.rtsp_port.c_str());
    mounts_ = gst_rtsp_server_get_mount_points(server_);
    if (!installFactory()) {
        std::cerr << "[rtsp] failed to install media factory\n";
        running_.store(false);
        g_object_unref(mounts_);
        mounts_ = nullptr;
        g_object_unref(server_);
        server_ = nullptr;
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        return false;
    }

    attach_id_ = gst_rtsp_server_attach(server_, nullptr);
    if (attach_id_ == 0) {
        std::cerr << "[rtsp] failed to bind " << config_.rtsp_address << ':' << config_.rtsp_port << '\n';
        running_.store(false);
        GstRTSPMediaFactory *factory = nullptr;
        gulong factory_handler = 0;
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            factory = current_factory_;
            factory_handler = current_factory_handler_;
            current_factory_ = nullptr;
            current_factory_handler_ = 0;
        }
        if (factory) {
            if (factory_handler != 0) {
                g_signal_handler_disconnect(factory, factory_handler);
            }
            gst_rtsp_mount_points_remove_factory(mounts_, config_.rtsp_mount.c_str());
            g_object_unref(factory);
        }
        g_object_unref(mounts_);
        mounts_ = nullptr;
        g_object_unref(server_);
        server_ = nullptr;
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        return false;
    }
    accepting_clients_ = true;

    status_since_us_ = g_get_monotonic_time();
    last_session_cleanup_us_ = status_since_us_;
    watchdog_id_ = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                      500,
                                      RtspServer::watchdogTick,
                                      newCallbackData(callback_state_),
                                      RtspServer::destroySourceCallbackData);
    feeder_thread_ = std::thread(&RtspServer::feederLoop, this);
    loop_thread_ = std::thread([this] { g_main_loop_run(loop_); });
    std::cout << "[rtsp] ready at rtsp://<device-ip>:" << config_.rtsp_port
              << config_.rtsp_mount << " (H264 High@L4, DMABUF import)\n";
    return true;
}

void RtspServer::submit(std::shared_ptr<FrameLease> frame) {
    if (!running_.load()) {
        return;
    }
    // A zero-copy GstBuffer owns its libcamera request until every downstream
    // element releases it. Always leave at least two requests available to the
    // camera so a pipeline state transition cannot deadlock capture by holding
    // the entire request pool.
    const std::uint64_t reserve = std::min<std::uint64_t>(2, config_.buffer_count / 2);
    if (metrics_.outstanding_leases.load() >= config_.buffer_count - reserve) {
        metrics_.rtsp_dropped.fetch_add(1);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (!appsrc_) {
            metrics_.rtsp_dropped.fetch_add(1);
            return;
        }
    }
    if (latest_.push(std::move(frame))) {
        metrics_.rtsp_dropped.fetch_add(1);
    }
}

void RtspServer::mediaConfigure(GstRTSPMediaFactory *factory,
                                GstRTSPMedia *media,
                                gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaConfigure(factory, media);
    }
}

void RtspServer::mediaPrepared(GstRTSPMedia *media, gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaPrepared(media);
    }
}

void RtspServer::mediaUnprepared(GstRTSPMedia *media, gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaUnprepared(media);
    }
}

void RtspServer::mediaTargetState(GstRTSPMedia *media, GstState state, gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaTargetState(media, state);
    }
}

void RtspServer::mediaNewState(GstRTSPMedia *media, GstState state, gpointer user_data) {
    CallbackGuard guard(user_data);
    if (guard.server()) {
        guard.server()->onMediaNewState(media, state);
    }
}

gboolean RtspServer::mediaHandleMessage(GstRTSPMedia *media,
                                        GstMessage *message,
                                        gpointer user_data) {
    CallbackGuard guard(user_data);
    return guard.server() ? guard.server()->onMediaHandleMessage(media, message) : FALSE;
}

gboolean RtspServer::watchdogTick(gpointer user_data) {
    CallbackGuard guard(user_data);
    return guard.server() ? guard.server()->onWatchdog() : G_SOURCE_REMOVE;
}

GstRTSPFilterResult RtspServer::closeClient(GstRTSPServer *,
                                             GstRTSPClient *client,
                                             gpointer) {
    // GST_RTSP_FILTER_REMOVE makes gst_rtsp_server_client_filter() close the
    // client after releasing the server lock. Do not close it a second time in
    // this callback.
    (void)client;
    return GST_RTSP_FILTER_REMOVE;
}

GstRTSPFilterResult RtspServer::removeSession(GstRTSPSessionPool *,
                                               GstRTSPSession *,
                                               gpointer) {
    return GST_RTSP_FILTER_REMOVE;
}

RtspServer::MediaHandlers RtspServer::connectMediaHandlers(GstRTSPMedia *media) {
    MediaHandlers handlers;
    handlers.prepared = g_signal_connect_data(
        media, "prepared", G_CALLBACK(RtspServer::mediaPrepared),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    handlers.unprepared = g_signal_connect_data(
        media, "unprepared", G_CALLBACK(RtspServer::mediaUnprepared),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        G_CONNECT_AFTER);
    handlers.target_state = g_signal_connect_data(
        media, "target-state", G_CALLBACK(RtspServer::mediaTargetState),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    handlers.new_state = g_signal_connect_data(
        media, "new-state", G_CALLBACK(RtspServer::mediaNewState),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    handlers.handle_message = g_signal_connect_data(
        media, "handle-message", G_CALLBACK(RtspServer::mediaHandleMessage),
        newCallbackData(callback_state_), RtspServer::destroySignalCallbackData,
        static_cast<GConnectFlags>(0));
    return handlers;
}

void RtspServer::disconnectMediaHandlers(GstRTSPMedia *media,
                                         const MediaHandlers &handlers) {
    if (!media) {
        return;
    }
    for (const gulong handler : {handlers.prepared,
                                 handlers.unprepared,
                                 handlers.target_state,
                                 handlers.new_state,
                                 handlers.handle_message}) {
        if (handler != 0 && g_signal_handler_is_connected(media, handler)) {
            g_signal_handler_disconnect(media, handler);
        }
    }
}

bool RtspServer::bindMediaSource(GstRTSPMedia *media) {
    GstElement *element = gst_rtsp_media_get_element(media);
    if (!element) {
        metrics_.rtsp_errors.fetch_add(1);
        std::cerr << "[rtsp] media pipeline has no root element\n";
        return false;
    }
    GstElement *source = gst_bin_get_by_name_recurse_up(GST_BIN(element), "source");
    gst_object_unref(element);
    if (!source || !GST_IS_APP_SRC(source)) {
        if (source) {
            gst_object_unref(source);
        }
        metrics_.rtsp_errors.fetch_add(1);
        std::cerr << "[rtsp] media pipeline has no appsrc\n";
        return false;
    }

    g_object_set(source,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", FALSE,
                 "block", FALSE,
                 nullptr);
    gst_app_src_set_stream_type(GST_APP_SRC(source), GST_APP_STREAM_TYPE_STREAM);
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                        "format", G_TYPE_STRING, "I420",
                                        "width", G_TYPE_INT, static_cast<int>(config_.main_width),
                                        "height", G_TYPE_INT, static_cast<int>(config_.main_height),
                                        "framerate", GST_TYPE_FRACTION,
                                        static_cast<int>(config_.fps), 1,
                                        "colorimetry", G_TYPE_STRING, "bt709",
                                        "interlace-mode", G_TYPE_STRING, "progressive",
                                        "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                                        nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);

    GstAppSrc *previous = nullptr;
    bool changed = false;
    bool accepted = true;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ != media || recovery_generation_ == media_generation_) {
            accepted = false;
        } else if (appsrc_ != GST_APP_SRC(source)) {
            previous = appsrc_;
            appsrc_ = GST_APP_SRC(source);  // gst_bin_get_by_name supplied this reference.
            generation_.fetch_add(1);
            changed = true;
        }
    }
    if (!accepted || !changed) {
        gst_object_unref(source);
    }
    if (previous) {
        gst_object_unref(previous);
    }
    if (changed) {
        latest_.clear();
        std::cout << "[rtsp] media source bound generation=" << generation_.load() << '\n';
    }
    return accepted;
}

void RtspServer::onMediaConfigure(GstRTSPMediaFactory *factory, GstRTSPMedia *media) {
    // The Pi's stateful V4L2 encoder retains imported DMABUFs when the same
    // pipeline is prepared again. Mark each media non-reusable so GStreamer's
    // stable shared factory evicts it on unprepare and constructs a fresh
    // pipeline for the next request without racing mount/factory replacement.
    gst_rtsp_media_set_reusable(media, FALSE);
    gst_rtsp_media_set_stop_on_disconnect(media, FALSE);
    gst_rtsp_media_set_eos_shutdown(media, FALSE);
    gst_rtsp_media_set_suspend_mode(media, GST_RTSP_SUSPEND_MODE_NONE);

    const MediaHandlers handlers = connectMediaHandlers(media);
    GstRTSPMedia *previous_media = nullptr;
    GstAppSrc *previous_source = nullptr;
    MediaHandlers previous_handlers;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        const bool replacing_retired_media =
            current_media_ != nullptr &&
            (observed_status_ == GST_RTSP_MEDIA_STATUS_UNPREPARING ||
             observed_status_ == GST_RTSP_MEDIA_STATUS_ERROR);
        if (running_.load() && current_factory_ == factory &&
            (current_media_ == nullptr || replacing_retired_media)) {
            previous_media = current_media_;
            previous_source = appsrc_;
            previous_handlers = current_media_handlers_;
            current_media_ = static_cast<GstRTSPMedia *>(g_object_ref(media));
            current_media_handlers_ = handlers;
            appsrc_ = nullptr;
            generation_.fetch_add(1);
            ++media_generation_;
            consecutive_push_failures_ = 0;
            recovery_generation_ = 0;
            recovery_reason_.clear();
            recovery_started_us_ = 0;
            recovery_failure_reported_ = false;
            observed_status_ = GST_RTSP_MEDIA_STATUS_PREPARING;
            status_since_us_ = g_get_monotonic_time();
            accepted = true;
        }
    }
    if (!accepted) {
        disconnectMediaHandlers(media, handlers);
        std::cout << "[rtsp] ignored concurrent media configuration\n";
        return;
    }
    disconnectMediaHandlers(previous_media, previous_handlers);
    if (previous_source) {
        gst_object_unref(previous_source);
    }
    if (previous_media) {
        g_object_unref(previous_media);
    }
    latest_.clear();
    if (!bindMediaSource(media)) {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ == media) {
            recovery_generation_ = media_generation_;
            recovery_reason_ = "media setup failure";
        }
        return;
    }
    std::cout << "[rtsp] client media configured\n";
}

void RtspServer::onMediaPrepared(GstRTSPMedia *media) {
    if (bindMediaSource(media)) {
        bool current = false;
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            if (current_media_ == media) {
                consecutive_push_failures_ = 0;
                observed_status_ = GST_RTSP_MEDIA_STATUS_PREPARED;
                status_since_us_ = g_get_monotonic_time();
                current = true;
            }
        }
        if (current) {
            std::cout << "[rtsp] client media prepared\n";
        }
    }
}

void RtspServer::onMediaUnprepared(GstRTSPMedia *media) {
    GstRTSPMedia *retired_media = nullptr;
    GstAppSrc *retired_source = nullptr;
    MediaHandlers retired_handlers;
    bool recovered_from_error = false;
    std::string recovery_reason;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ == media) {
            retired_media = current_media_;
            retired_source = appsrc_;
            retired_handlers = current_media_handlers_;
            recovered_from_error = recovery_generation_ == media_generation_;
            recovery_reason = recovery_reason_;
            current_media_ = nullptr;
            current_media_handlers_ = {};
            appsrc_ = nullptr;
            generation_.fetch_add(1);
            ++media_generation_;
            consecutive_push_failures_ = 0;
            recovery_generation_ = 0;
            recovery_reason_.clear();
            recovery_started_us_ = 0;
            recovery_failure_reported_ = false;
            observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
            status_since_us_ = g_get_monotonic_time();
        }
    }
    if (!retired_media) {
        std::cout << "[rtsp] ignored stale media release\n";
        return;
    }
    latest_.clear();
    disconnectMediaHandlers(retired_media, retired_handlers);
    if (retired_source) {
        gst_object_unref(retired_source);
    }
    g_object_unref(retired_media);
    if (recovered_from_error) {
        guint resumed_attach_id = 0;
        if (running_.load()) {
            resumed_attach_id = gst_rtsp_server_attach(server_, nullptr);
        }
        bool resumed = false;
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            if (running_.load() && !accepting_clients_ && resumed_attach_id != 0) {
                attach_id_ = resumed_attach_id;
                accepting_clients_ = true;
                resumed = true;
            }
        }
        if (!resumed && resumed_attach_id != 0) {
            g_source_remove(resumed_attach_id);
        }
        if (resumed) {
            metrics_.rtsp_recoveries.fetch_add(1);
            std::cerr << "[rtsp] recovery completed; media cache cleared and listener resumed: "
                      << recovery_reason << '\n';
        } else if (running_.load()) {
            metrics_.rtsp_errors.fetch_add(1);
            metrics_.rtsp_recovery_failures.fetch_add(1);
            std::cerr << "[rtsp] recovery failed: media cache cleared but listener did not resume\n";
        }
    } else {
        std::cout << "[rtsp] client media released; factory cache cleared\n";
    }
}

void RtspServer::onMediaTargetState(GstRTSPMedia *media, GstState state) {
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ != media || state != GST_STATE_NULL) {
            return;
        }
        observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARING;
        status_since_us_ = g_get_monotonic_time();
    }
    std::cout << "[rtsp] media status=unpreparing\n";
}

void RtspServer::onMediaNewState(GstRTSPMedia *media, GstState state) {
    if (state == GST_STATE_READY || state == GST_STATE_PAUSED || state == GST_STATE_PLAYING) {
        bindMediaSource(media);
    }
    std::cout << "[rtsp] media state=" << gst_element_state_get_name(state) << '\n';
}

gboolean RtspServer::onMediaHandleMessage(GstRTSPMedia *media, GstMessage *message) {
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR) {
        return FALSE;
    }

    bool first_request = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ != media) {
            return FALSE;
        }
        first_request = recovery_generation_ != media_generation_;
        recovery_generation_ = media_generation_;
        recovery_reason_ = "media bus reported an error";
        observed_status_ = GST_RTSP_MEDIA_STATUS_ERROR;
        status_since_us_ = g_get_monotonic_time();
    }
    if (!first_request) {
        return FALSE;
    }

    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    metrics_.rtsp_errors.fetch_add(1);
    std::cerr << "[rtsp] media error: "
              << (error && error->message ? error->message : "unknown error");
    if (debug && *debug) {
        std::cerr << " (" << debug << ')';
    }
    std::cerr << '\n';
    g_clear_error(&error);
    g_free(debug);

    // Returning FALSE lets GstRTSPMedia's default run-last handler continue
    // processing the bus message after we have scheduled recovery.
    return FALSE;
}

gboolean RtspServer::onWatchdog() {
    if (!running_.load()) {
        return G_SOURCE_REMOVE;
    }

    const gint64 now = g_get_monotonic_time();
    if (now - last_session_cleanup_us_ >= 5 * G_USEC_PER_SEC) {
        GstRTSPSessionPool *pool = gst_rtsp_server_get_session_pool(server_);
        const guint removed = gst_rtsp_session_pool_cleanup(pool);
        g_object_unref(pool);
        if (removed > 0) {
            std::cout << "[rtsp] cleaned " << removed << " expired session(s)\n";
        }
        last_session_cleanup_us_ = now;
    }

    std::uint64_t media_generation = 0;
    std::uint64_t recovery_generation = 0;
    std::string recovery_reason;
    GstRTSPMediaStatus status = GST_RTSP_MEDIA_STATUS_UNPREPARED;
    gint64 status_since_us = now;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_) {
            media_generation = media_generation_;
            recovery_generation = recovery_generation_;
            recovery_reason = recovery_reason_;
            status = observed_status_;
            status_since_us = status_since_us_;
        }
    }

    if (recovery_generation != 0) {
        recoverMedia(recovery_generation,
                     recovery_reason.empty() ? "media recovery requested"
                                             : recovery_reason.c_str());
        return G_SOURCE_CONTINUE;
    }

    if (media_generation == 0) {
        return G_SOURCE_CONTINUE;
    }

    const gint64 status_age = now - status_since_us;
    if (status == GST_RTSP_MEDIA_STATUS_ERROR) {
        recoverMedia(media_generation, "media entered error state");
    } else if (status == GST_RTSP_MEDIA_STATUS_UNPREPARING &&
               status_age >= 3 * G_USEC_PER_SEC) {
        recoverMedia(media_generation, "media remained unpreparing for 3 seconds");
    } else if (status == GST_RTSP_MEDIA_STATUS_PREPARING &&
               status_age >= 10 * G_USEC_PER_SEC) {
        recoverMedia(media_generation, "media remained preparing for 10 seconds");
    }
    return G_SOURCE_CONTINUE;
}

bool RtspServer::recoverMedia(std::uint64_t expected_media_generation, const char *reason) {
    GstAppSrc *source = nullptr;
    std::uint64_t actual_media_generation = 0;
    guint listener_source = 0;
    bool begin_recovery = false;
    bool report_timeout = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        actual_media_generation = current_media_ ? media_generation_ : 0;
        if (!current_media_ || media_generation_ != expected_media_generation) {
            if (recovery_generation_ == expected_media_generation) {
                recovery_generation_ = 0;
                recovery_reason_.clear();
            }
        } else if (recovery_started_us_ == 0) {
            source = appsrc_;
            appsrc_ = nullptr;
            generation_.fetch_add(1);
            consecutive_push_failures_ = 0;
            recovery_generation_ = expected_media_generation;
            recovery_reason_ = reason;
            recovery_started_us_ = g_get_monotonic_time();
            recovery_failure_reported_ = false;
            status_since_us_ = g_get_monotonic_time();
            listener_source = attach_id_;
            accepting_clients_ = false;
            begin_recovery = true;
        } else if (!recovery_failure_reported_ &&
                   g_get_monotonic_time() - recovery_started_us_ >= 5 * G_USEC_PER_SEC) {
            recovery_failure_reported_ = true;
            report_timeout = true;
        }
    }
    if (!begin_recovery && !report_timeout) {
        if (actual_media_generation == expected_media_generation) {
            return false;
        }
        std::cout << "[rtsp] ignored stale recovery request generation="
                  << expected_media_generation << " current=" << actual_media_generation << '\n';
        return false;
    }
    if (report_timeout) {
        metrics_.rtsp_errors.fetch_add(1);
        metrics_.rtsp_recovery_failures.fetch_add(1);
        std::cerr << "[rtsp] recovery failed: media did not reach UNPREPARED within 5 seconds"
                  << " generation=" << expected_media_generation << '\n';
        return false;
    }

    latest_.clear();
    if (listener_source != 0) {
        g_source_remove(listener_source);
    }
    if (source) {
        gst_app_src_end_of_stream(source);
        gst_object_unref(source);
    }

    // Every successful prepare is owned either by a client's cached media or
    // by a GstRTSPSessionMedia. Close clients and remove sessions through the
    // public server APIs so each owner performs its matching unprepare. The
    // last owner emits "unprepared"; our G_CONNECT_AFTER handler then runs
    // after the factory has evicted this non-reusable media from its cache.
    if (server_) {
        GList *clients = gst_rtsp_server_client_filter(server_, RtspServer::closeClient, nullptr);
        g_list_free_full(clients, g_object_unref);
        GstRTSPSessionPool *pool = gst_rtsp_server_get_session_pool(server_);
        GList *sessions = gst_rtsp_session_pool_filter(pool, RtspServer::removeSession, nullptr);
        g_list_free_full(sessions, g_object_unref);
        g_object_unref(pool);
    }
    std::cerr << "[rtsp] recovery teardown started: " << reason << '\n';
    return true;
}

GstBuffer *RtspServer::makeBuffer(std::shared_ptr<FrameLease> frame,
                                  std::uint64_t base_timestamp,
                                  std::uint64_t frame_index) const {
    const StreamView &view = frame->main();
    if (view.planes.empty()) {
        return nullptr;
    }

    GstBuffer *buffer = gst_buffer_new();
    GstAllocator *allocator = gst_dmabuf_allocator_new();
    if (!buffer || !allocator) {
        if (buffer) {
            gst_buffer_unref(buffer);
        }
        if (allocator) {
            gst_object_unref(allocator);
        }
        return nullptr;
    }

    const bool shared_fd = std::all_of(view.planes.begin(),
                                       view.planes.end(),
                                       [&view](const PlaneView &plane) {
                                           return plane.fd == view.planes.front().fd;
                                       });
    if (shared_fd) {
        const PlaneView &plane = view.planes.front();
        const int owned_fd = dup(plane.fd);
        std::size_t allocated_size = 0;
        for (const PlaneView &candidate : view.planes) {
            allocated_size = std::max(allocated_size,
                                      static_cast<std::size_t>(candidate.offset) + candidate.length);
        }
        allocated_size = std::max(allocated_size, static_cast<std::size_t>(view.frame_size));
        GstMemory *memory = owned_fd >= 0
                                ? gst_dmabuf_allocator_alloc(allocator, owned_fd, allocated_size)
                                : nullptr;
        if (!memory) {
            if (owned_fd >= 0) {
                close(owned_fd);
            }
            gst_object_unref(allocator);
            gst_buffer_unref(buffer);
            return nullptr;
        }
        gst_buffer_append_memory(buffer, memory);
    } else if (view.planes.size() == 3) {
        for (const PlaneView &plane : view.planes) {
            const int owned_fd = dup(plane.fd);
            const std::size_t allocated_size = static_cast<std::size_t>(plane.offset) + plane.length;
            GstMemory *memory = owned_fd >= 0
                                    ? gst_dmabuf_allocator_alloc(allocator, owned_fd, allocated_size)
                                    : nullptr;
            if (!memory) {
                if (owned_fd >= 0) {
                    close(owned_fd);
                }
                gst_object_unref(allocator);
                gst_buffer_unref(buffer);
                return nullptr;
            }
            if (plane.offset != 0) {
                gst_memory_resize(memory, plane.offset, plane.length);
            }
            gst_buffer_append_memory(buffer, memory);
        }
    } else {
        gst_object_unref(allocator);
        gst_buffer_unref(buffer);
        return nullptr;
    }
    gst_object_unref(allocator);

    std::array<gsize, GST_VIDEO_MAX_PLANES> offsets{};
    std::array<gint, GST_VIDEO_MAX_PLANES> strides{};
    strides[0] = static_cast<gint>(view.stride);
    strides[1] = static_cast<gint>(view.stride / 2);
    strides[2] = static_cast<gint>(view.stride / 2);
    if (shared_fd && view.planes.size() == 3) {
        offsets[0] = view.planes[0].offset;
        offsets[1] = view.planes[1].offset;
        offsets[2] = view.planes[2].offset;
    } else {
        offsets[0] = 0;
        offsets[1] = static_cast<gsize>(view.stride) * view.height;
        offsets[2] = offsets[1] + static_cast<gsize>(view.stride / 2) * (view.height / 2);
    }
    gst_buffer_add_video_meta_full(buffer,
                                   GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_I420,
                                   view.width,
                                   view.height,
                                   3,
                                   offsets.data(),
                                   strides.data());

    const std::uint64_t timestamp = frame->sensorTimestampNs();
    const GstClockTime pts = timestamp >= base_timestamp
                                 ? static_cast<GstClockTime>(timestamp - base_timestamp)
                                 : frame_index * GST_SECOND / config_.fps;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / config_.fps;
    GST_BUFFER_OFFSET(buffer) = frame_index;
    GST_BUFFER_OFFSET_END(buffer) = frame_index + 1;
    GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_LIVE);

    auto *holder = new std::shared_ptr<FrameLease>(std::move(frame));
    gst_mini_object_set_qdata(GST_MINI_OBJECT(buffer), leaseQuark(), holder, destroyLease);
    return buffer;
}

void RtspServer::feederLoop() {
    std::uint64_t observed_generation = generation_.load();
    std::uint64_t base_timestamp = 0;
    std::uint64_t frame_index = 0;
    while (running_.load()) {
        std::shared_ptr<FrameLease> frame;
        if (!latest_.waitPop(frame)) {
            break;
        }

        GstAppSrc *source = nullptr;
        std::uint64_t generation = 0;
        std::uint64_t media_generation = 0;
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            generation = generation_.load();
            media_generation = media_generation_;
            if (appsrc_) {
                source = GST_APP_SRC(gst_object_ref(appsrc_));
            }
        }
        if (!source) {
            metrics_.rtsp_dropped.fetch_add(1);
            continue;
        }
        if (generation != observed_generation || base_timestamp == 0) {
            observed_generation = generation;
            base_timestamp = frame->sensorTimestampNs();
            frame_index = 0;
        }

        GstBuffer *buffer = makeBuffer(frame, base_timestamp, frame_index++);
        if (!buffer) {
            metrics_.rtsp_errors.fetch_add(1);
            gst_object_unref(source);
            continue;
        }
#ifdef BSAPS_ENABLE_TEST_HOOKS
        if (!test_push_error_consumed_ && !test_push_error_trigger_.empty() &&
            access(test_push_error_trigger_.c_str(), F_OK) == 0) {
            test_push_error_consumed_ = true;
            test_push_errors_remaining_ = 3;
            std::cerr << "[rtsp] test hook injecting three appsrc push errors\n";
        }
        GstFlowReturn flow = GST_FLOW_OK;
        if (test_push_errors_remaining_ > 0) {
            --test_push_errors_remaining_;
            gst_buffer_unref(buffer);
            flow = GST_FLOW_ERROR;
        } else {
            flow = gst_app_src_push_buffer(source, buffer);
        }
#else
        const GstFlowReturn flow = gst_app_src_push_buffer(source, buffer);
#endif
        gst_object_unref(source);
        if (flow == GST_FLOW_OK) {
            metrics_.rtsp_pushed.fetch_add(1);
            std::lock_guard<std::mutex> lock(source_mutex_);
            if (current_media_ && media_generation_ == media_generation) {
                consecutive_push_failures_ = 0;
            }
        } else if (flow != GST_FLOW_FLUSHING) {
            metrics_.rtsp_errors.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(source_mutex_);
                if (current_media_ && media_generation_ == media_generation) {
                    ++consecutive_push_failures_;
                    if (consecutive_push_failures_ >= 3) {
                        recovery_generation_ = media_generation;
                        recovery_reason_ = "repeated appsrc push failure";
                    }
                }
            }
            std::cerr << "[rtsp] appsrc push failed: " << gst_flow_get_name(flow) << '\n';
        } else {
            std::lock_guard<std::mutex> lock(source_mutex_);
            if (current_media_ && media_generation_ == media_generation) {
                consecutive_push_failures_ = 0;
            }
        }
    }
}

void RtspServer::disableCallbacksAndWait() {
    std::unique_lock<std::mutex> lock(callback_state_->mutex);
    callback_state_->server = nullptr;
    callback_state_->cv.wait(lock, [this] { return callback_state_->active == 0; });
}

void RtspServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    latest_.close();
    if (feeder_thread_.joinable()) {
        feeder_thread_.join();
    }
    if (watchdog_id_ != 0) {
        g_source_remove(watchdog_id_);
        watchdog_id_ = 0;
    }
    if (attach_id_ != 0) {
        g_source_remove(attach_id_);
        attach_id_ = 0;
    }
    accepting_clients_ = false;

    // Media signals run on GstRTSPThread contexts, not necessarily loop_thread_.
    // Make every callback drop its independently-owned state reference, then
    // wait for callbacks that already captured this server to finish before
    // releasing any GStreamer object or the server itself.
    disableCallbacksAndWait();

    GstAppSrc *source = nullptr;
    GstRTSPMedia *media = nullptr;
    MediaHandlers media_handlers;
    GstRTSPMediaFactory *factory = nullptr;
    gulong factory_handler = 0;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        source = appsrc_;
        media = current_media_;
        media_handlers = current_media_handlers_;
        factory = current_factory_;
        factory_handler = current_factory_handler_;
        appsrc_ = nullptr;
        current_media_ = nullptr;
        current_media_handlers_ = {};
        current_factory_ = nullptr;
        current_factory_handler_ = 0;
        generation_.fetch_add(1);
        ++media_generation_;
        consecutive_push_failures_ = 0;
        recovery_generation_ = 0;
        recovery_reason_.clear();
        recovery_started_us_ = 0;
        recovery_failure_reported_ = false;
        observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
        status_since_us_ = g_get_monotonic_time();
    }

    disconnectMediaHandlers(media, media_handlers);
    if (factory && factory_handler != 0 &&
        g_signal_handler_is_connected(factory, factory_handler)) {
        g_signal_handler_disconnect(factory, factory_handler);
    }
    if (mounts_) {
        gst_rtsp_mount_points_remove_factory(mounts_, config_.rtsp_mount.c_str());
    }
    if (server_) {
        GList *clients = gst_rtsp_server_client_filter(server_, RtspServer::closeClient, nullptr);
        g_list_free_full(clients, g_object_unref);
    }

    if (source) {
        gst_app_src_end_of_stream(source);
    }
    if (media) {
        gst_rtsp_media_set_pipeline_state(media, GST_STATE_NULL);
        gst_rtsp_media_unprepare(media);
    }
    if (source) {
        gst_object_unref(source);
    }
    if (loop_) {
        g_main_loop_quit(loop_);
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    if (media) {
        g_object_unref(media);
    }
    if (factory) {
        g_object_unref(factory);
    }
    if (mounts_) {
        g_object_unref(mounts_);
        mounts_ = nullptr;
    }
    if (server_) {
        g_object_unref(server_);
        server_ = nullptr;
    }
    if (loop_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }
    std::cout << "[rtsp] stopped\n";
}

std::string RtspServer::url(const std::string &host) const {
    return "rtsp://" + host + ':' + config_.rtsp_port + config_.rtsp_mount;
}

}  // namespace bsaps
