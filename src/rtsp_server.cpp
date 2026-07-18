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

bool RtspServer::start() {
    if (running_.exchange(true)) {
        return true;
    }

    loop_ = g_main_loop_new(nullptr, FALSE);
    server_ = gst_rtsp_server_new();
    gst_rtsp_server_set_address(server_, config_.rtsp_address.c_str());
    gst_rtsp_server_set_service(server_, config_.rtsp_port.c_str());

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server_);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    std::ostringstream pipeline;
    pipeline << "( appsrc name=source is-live=true format=time do-timestamp=false block=false "
             << "max-buffers=2 leaky-type=downstream "
             // GStreamer 1.22's v4l2 encoder imports GstDmaBufMemory but its pad
             // template advertises plain video/x-raw. Adding the memory feature
             // causes a not-linked error; the memory object remains DMABUF here.
             << "! video/x-raw,format=I420,width=" << config_.main_width
             << ",height=" << config_.main_height << ",framerate=" << config_.fps
             << "/1,colorimetry=bt709,interlace-mode=progressive,pixel-aspect-ratio=1/1 "
             << "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream "
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
    gst_rtsp_media_factory_set_stop_on_disconnect(factory, TRUE);
    gst_rtsp_media_factory_set_eos_shutdown(factory, TRUE);
    gst_rtsp_media_factory_set_protocols(
        factory, static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));
    gst_rtsp_media_factory_set_latency(factory, 50);
    g_signal_connect(factory, "media-configure", G_CALLBACK(RtspServer::mediaConfigure), this);
    gst_rtsp_mount_points_add_factory(mounts, config_.rtsp_mount.c_str(), factory);
    g_object_unref(mounts);

    attach_id_ = gst_rtsp_server_attach(server_, nullptr);
    if (attach_id_ == 0) {
        std::cerr << "[rtsp] failed to bind " << config_.rtsp_address << ':' << config_.rtsp_port << '\n';
        running_.store(false);
        g_object_unref(server_);
        server_ = nullptr;
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        return false;
    }

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

void RtspServer::mediaUnprepared(GstRTSPMedia *, gpointer user_data) {
    static_cast<RtspServer *>(user_data)->onMediaUnprepared();
}

void RtspServer::onMediaConfigure(GstRTSPMedia *media) {
    GstElement *element = gst_rtsp_media_get_element(media);
    GstElement *source = gst_bin_get_by_name_recurse_up(GST_BIN(element), "source");
    gst_object_unref(element);
    if (!source || !GST_IS_APP_SRC(source)) {
        if (source) {
            gst_object_unref(source);
        }
        metrics_.rtsp_errors.fetch_add(1);
        std::cerr << "[rtsp] media pipeline has no appsrc\n";
        return;
    }

    g_object_set(source,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", FALSE,
                 "block", FALSE,
                 nullptr);
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
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (appsrc_) {
            gst_object_unref(appsrc_);
        }
        appsrc_ = GST_APP_SRC(source);  // gst_bin_get_by_name supplied this reference.
        generation_.fetch_add(1);
    }
    latest_.clear();
    g_signal_connect(media, "unprepared", G_CALLBACK(RtspServer::mediaUnprepared), this);
    std::cout << "[rtsp] client media prepared\n";
}

void RtspServer::onMediaUnprepared() {
    latest_.clear();
    std::lock_guard<std::mutex> lock(source_mutex_);
    if (appsrc_) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    generation_.fetch_add(1);
    std::cout << "[rtsp] client media released\n";
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
        {
            std::lock_guard<std::mutex> lock(source_mutex_);
            generation = generation_.load();
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
        } else if (flow != GST_FLOW_FLUSHING) {
            metrics_.rtsp_errors.fetch_add(1);
            std::cerr << "[rtsp] appsrc push failed: " << gst_flow_get_name(flow) << '\n';
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
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (appsrc_) {
            gst_app_src_end_of_stream(appsrc_);
            gst_object_unref(appsrc_);
            appsrc_ = nullptr;
        }
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
