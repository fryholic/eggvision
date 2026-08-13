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

RtspServer::RtspServer(const AppConfig &config, Metrics &metrics)
    : config_(config), metrics_(metrics) {
    gst_init(nullptr, nullptr);
}

RtspServer::~RtspServer() {
    stop();
}

bool RtspServer::installFactory() {
    if (!mounts_) {
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
    g_signal_connect(factory, "media-configure", G_CALLBACK(RtspServer::mediaConfigure), this);
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
        g_object_unref(mounts_);
        mounts_ = nullptr;
        g_object_unref(server_);
        server_ = nullptr;
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        return false;
    }

    status_since_us_ = g_get_monotonic_time();
    last_session_cleanup_us_ = status_since_us_;
    watchdog_id_ = g_timeout_add(500, RtspServer::watchdogTick, this);
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

void RtspServer::mediaConfigure(GstRTSPMediaFactory *, GstRTSPMedia *media, gpointer user_data) {
    static_cast<RtspServer *>(user_data)->onMediaConfigure(media);
}

void RtspServer::mediaPrepared(GstRTSPMedia *media, gpointer user_data) {
    static_cast<RtspServer *>(user_data)->onMediaPrepared(media);
}

void RtspServer::mediaUnprepared(GstRTSPMedia *media, gpointer user_data) {
    static_cast<RtspServer *>(user_data)->onMediaUnprepared(media);
}

void RtspServer::mediaTargetState(GstRTSPMedia *media, GstState state, gpointer user_data) {
    static_cast<RtspServer *>(user_data)->onMediaTargetState(media, state);
}

void RtspServer::mediaNewState(GstRTSPMedia *media, GstState state, gpointer user_data) {
    static_cast<RtspServer *>(user_data)->onMediaNewState(media, state);
}

gboolean RtspServer::mediaHandleMessage(GstRTSPMedia *media,
                                        GstMessage *message,
                                        gpointer user_data) {
    return static_cast<RtspServer *>(user_data)->onMediaHandleMessage(media, message);
}

gboolean RtspServer::watchdogTick(gpointer user_data) {
    return static_cast<RtspServer *>(user_data)->onWatchdog();
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
        if (current_media_ != media) {
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

void RtspServer::onMediaConfigure(GstRTSPMedia *media) {
    // The Pi's stateful V4L2 encoder retains imported DMABUFs when the same
    // pipeline is prepared again. Retire it completely after unprepare and
    // install a fresh shared factory instead of reusing encoder state.
    gst_rtsp_media_set_reusable(media, FALSE);
    gst_rtsp_media_set_stop_on_disconnect(media, FALSE);
    gst_rtsp_media_set_eos_shutdown(media, FALSE);
    gst_rtsp_media_set_suspend_mode(media, GST_RTSP_SUSPEND_MODE_NONE);

    GstRTSPMedia *previous_media = nullptr;
    GstAppSrc *previous_source = nullptr;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ != media) {
            previous_media = current_media_;
            previous_source = appsrc_;
            current_media_ = static_cast<GstRTSPMedia *>(g_object_ref(media));
            appsrc_ = nullptr;
            generation_.fetch_add(1);
            ++media_generation_;
            consecutive_push_failures_ = 0;
            recovery_generation_ = 0;
            recovery_reason_.clear();
            observed_status_ = GST_RTSP_MEDIA_STATUS_PREPARING;
            status_since_us_ = g_get_monotonic_time();
            changed = true;
        }
    }
    if (previous_source) {
        gst_object_unref(previous_source);
    }
    if (previous_media) {
        g_signal_handlers_disconnect_by_data(previous_media, this);
        g_object_unref(previous_media);
    }
    if (changed) {
        latest_.clear();
        g_signal_connect(media, "prepared", G_CALLBACK(RtspServer::mediaPrepared), this);
        g_signal_connect(media, "unprepared", G_CALLBACK(RtspServer::mediaUnprepared), this);
        g_signal_connect(media, "target-state", G_CALLBACK(RtspServer::mediaTargetState), this);
        g_signal_connect(media, "new-state", G_CALLBACK(RtspServer::mediaNewState), this);
        g_signal_connect(media, "handle-message", G_CALLBACK(RtspServer::mediaHandleMessage), this);
    }
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
    bool recovered_from_error = false;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_media_ == media) {
            retired_media = current_media_;
            retired_source = appsrc_;
            recovered_from_error = recovery_generation_ == media_generation_;
            current_media_ = nullptr;
            appsrc_ = nullptr;
            generation_.fetch_add(1);
            ++media_generation_;
            consecutive_push_failures_ = 0;
            recovery_generation_ = 0;
            recovery_reason_.clear();
            observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
            status_since_us_ = g_get_monotonic_time();
        }
    }
    if (!retired_media) {
        std::cout << "[rtsp] ignored stale media release\n";
        return;
    }
    latest_.clear();
    g_signal_handlers_disconnect_by_data(retired_media, this);
    if (retired_source) {
        gst_object_unref(retired_source);
    }
    g_object_unref(retired_media);
    if (installFactory()) {
        if (recovered_from_error) {
            metrics_.rtsp_recoveries.fetch_add(1);
            std::cerr << "[rtsp] recovered media factory after pipeline error\n";
        } else {
            std::cout << "[rtsp] client media released; fresh factory installed\n";
        }
    } else {
        metrics_.rtsp_errors.fetch_add(1);
        std::cerr << "[rtsp] failed to refresh media factory after release\n";
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
    GstRTSPMedia *media = nullptr;
    GstAppSrc *source = nullptr;
    std::uint64_t actual_media_generation = 0;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        actual_media_generation = current_media_ ? media_generation_ : 0;
        if (!current_media_ || media_generation_ != expected_media_generation) {
            if (recovery_generation_ == expected_media_generation) {
                recovery_generation_ = 0;
                recovery_reason_.clear();
            }
        } else {
            media = current_media_;
            source = appsrc_;
            current_media_ = nullptr;
            appsrc_ = nullptr;
            generation_.fetch_add(1);
            ++media_generation_;
            consecutive_push_failures_ = 0;
            recovery_generation_ = 0;
            recovery_reason_.clear();
            observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
            status_since_us_ = g_get_monotonic_time();
        }
    }
    if (!media) {
        std::cout << "[rtsp] ignored stale recovery request generation="
                  << expected_media_generation << " current=" << actual_media_generation << '\n';
        return false;
    }
    latest_.clear();

    if (media) {
        g_signal_handlers_disconnect_by_data(media, this);
    }
    if (source) {
        gst_app_src_end_of_stream(source);
        gst_object_unref(source);
    }
    if (media) {
        gst_rtsp_media_unprepare(media);
        g_object_unref(media);
    }

    if (!installFactory()) {
        metrics_.rtsp_errors.fetch_add(1);
        std::cerr << "[rtsp] recovery failed to replace media factory\n";
        return false;
    }
    metrics_.rtsp_recoveries.fetch_add(1);
    std::cerr << "[rtsp] recovered media factory: " << reason << '\n';
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
        const GstFlowReturn flow = gst_app_src_push_buffer(source, buffer);
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
    GstAppSrc *source = nullptr;
    GstRTSPMedia *media = nullptr;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        source = appsrc_;
        media = current_media_;
        appsrc_ = nullptr;
        current_media_ = nullptr;
        generation_.fetch_add(1);
        ++media_generation_;
        consecutive_push_failures_ = 0;
        recovery_generation_ = 0;
        recovery_reason_.clear();
        observed_status_ = GST_RTSP_MEDIA_STATUS_UNPREPARED;
        status_since_us_ = g_get_monotonic_time();
    }
    if (media) {
        g_signal_handlers_disconnect_by_data(media, this);
    }
    if (source) {
        gst_app_src_end_of_stream(source);
        gst_object_unref(source);
    }
    if (loop_) {
        g_main_loop_quit(loop_);
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    if (attach_id_ != 0) {
        g_source_remove(attach_id_);
        attach_id_ = 0;
    }
    if (media) {
        g_object_unref(media);
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
