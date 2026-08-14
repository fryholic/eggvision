#include "eggvision/h264_encoder.hpp"

#include "eggvision/h264_bitstream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <system_error>
#include <unistd.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

namespace eggvision {
namespace {

GQuark leaseQuark() {
    static const GQuark quark = g_quark_from_static_string("eggvision-encoder-frame-lease");
    return quark;
}

void destroyLease(gpointer data) {
    delete static_cast<std::shared_ptr<FrameLease> *>(data);
}

}  // namespace

H264Encoder::H264Encoder(const AppConfig &config, Metrics &metrics)
    : config_(config), metrics_(metrics) {
    gst_init(nullptr, nullptr);
}

H264Encoder::~H264Encoder() {
    stop();
}

void H264Encoder::setConsumer(Consumer consumer) {
    std::lock_guard<std::mutex> lock(consumer_mutex_);
    consumer_ = std::move(consumer);
}

bool H264Encoder::initialize() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (initialized_.load()) {
        return true;
    }

    std::ostringstream launch;
    launch << "appsrc name=raw_source is-live=true format=time do-timestamp=false block=false "
           << "max-buffers=1 leaky-type=downstream "
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
           << "! h264parse config-interval=-1 "
           << "! video/x-h264,stream-format=byte-stream,alignment=au "
           << "! appsink name=encoded_sink sync=false max-buffers=8 drop=true";

    GError *error = nullptr;
    pipeline_ = gst_parse_launch(launch.str().c_str(), &error);
    if (!pipeline_) {
        std::cerr << "[encoder] pipeline creation failed: "
                  << (error && error->message ? error->message : "unknown error") << '\n';
        g_clear_error(&error);
        metrics_.encoder_errors.fetch_add(1);
        return false;
    }
    if (error) {
        std::cerr << "[encoder] pipeline parse warning: " << error->message << '\n';
        g_clear_error(&error);
    }

    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline_), "raw_source");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline_), "encoded_sink");
    if (!source || !sink || !GST_IS_APP_SRC(source) || !GST_IS_APP_SINK(sink)) {
        if (source) {
            gst_object_unref(source);
        }
        if (sink) {
            gst_object_unref(sink);
        }
        std::cerr << "[encoder] pipeline is missing appsrc or appsink\n";
        metrics_.encoder_errors.fetch_add(1);
        releasePipeline();
        return false;
    }
    appsrc_ = GST_APP_SRC(source);
    appsink_ = GST_APP_SINK(sink);
    gst_app_src_set_stream_type(appsrc_, GST_APP_STREAM_TYPE_STREAM);
    gst_app_sink_set_emit_signals(appsink_, FALSE);
    gst_app_sink_set_drop(appsink_, TRUE);
    gst_app_sink_set_max_buffers(appsink_, 8);
    initialized_.store(true);
    std::cout << "[encoder] initialized H264 High@L4 " << config_.main_width << 'x'
              << config_.main_height << '@' << config_.fps << " bitrate=" << config_.bitrate
              << " gop=" << config_.gop << '\n';
    return true;
}

bool H264Encoder::start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!initialized_.load() || !pipeline_ || !appsrc_ || !appsink_) {
        return false;
    }
    if (running_.exchange(true)) {
        return true;
    }

    latest_.reopen();
    base_sensor_timestamp_ns_.store(0);
    recovery_requested_.store(false);
    generation_.fetch_add(1);
    {
        std::lock_guard<std::mutex> ready_lock(ready_mutex_);
        independent_frame_seen_ = false;
    }
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[encoder] failed to enter PLAYING state\n";
        metrics_.encoder_errors.fetch_add(1);
        running_.store(false);
        return false;
    }

    try {
        input_thread_ = std::thread(&H264Encoder::inputLoop, this);
        output_thread_ = std::thread(&H264Encoder::outputLoop, this);
        bus_thread_ = std::thread(&H264Encoder::busLoop, this);
    } catch (const std::system_error &error) {
        std::cerr << "[encoder] failed to start worker: " << error.what() << '\n';
        metrics_.encoder_errors.fetch_add(1);
        running_.store(false);
        latest_.close();
        if (input_thread_.joinable()) {
            input_thread_.join();
        }
        if (output_thread_.joinable()) {
            output_thread_.join();
        }
        if (bus_thread_.joinable()) {
            bus_thread_.join();
        }
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        return false;
    }
    std::cout << "[encoder] started generation=" << generation_.load() << '\n';
    return true;
}

void H264Encoder::submit(std::shared_ptr<FrameLease> frame) {
    if (!running_.load() || !frame) {
        return;
    }
    const std::uint64_t reserve = std::min<std::uint64_t>(2, config_.buffer_count / 2);
    if (metrics_.outstanding_leases.load() >= config_.buffer_count - reserve) {
        metrics_.encoder_dropped.fetch_add(1);
        return;
    }
    if (latest_.push(std::move(frame))) {
        metrics_.encoder_dropped.fetch_add(1);
    }
}

bool H264Encoder::waitForIndependentFrame(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(ready_mutex_);
    return ready_cv_.wait_for(lock, timeout, [this] {
        return independent_frame_seen_ || !running_.load();
    }) && independent_frame_seen_;
}

GstBuffer *H264Encoder::makeRawBuffer(std::shared_ptr<FrameLease> frame,
                                      std::uint64_t base_sensor_timestamp_ns,
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
        const int owned_fd = dup(view.planes.front().fd);
        std::size_t allocated_size = view.frame_size;
        for (const PlaneView &plane : view.planes) {
            allocated_size = std::max(allocated_size,
                                      static_cast<std::size_t>(plane.offset) + plane.length);
        }
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
            const std::size_t allocated_size =
                static_cast<std::size_t>(plane.offset) + plane.length;
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
    const GstClockTime pts = timestamp >= base_sensor_timestamp_ns
                                 ? timestamp - base_sensor_timestamp_ns
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

void H264Encoder::inputLoop() {
    std::uint64_t frame_index = 0;
    while (running_.load()) {
        std::shared_ptr<FrameLease> frame;
        if (!latest_.waitPop(frame)) {
            break;
        }
        std::uint64_t base = base_sensor_timestamp_ns_.load();
        if (base == 0) {
            base = frame->sensorTimestampNs();
            base_sensor_timestamp_ns_.store(base);
            frame_index = 0;
        }
        GstBuffer *buffer = makeRawBuffer(std::move(frame), base, frame_index++);
        if (!buffer) {
            metrics_.encoder_errors.fetch_add(1);
            continue;
        }
        const GstFlowReturn flow = gst_app_src_push_buffer(appsrc_, buffer);
        if (flow != GST_FLOW_OK && flow != GST_FLOW_FLUSHING) {
            metrics_.encoder_errors.fetch_add(1);
            std::cerr << "[encoder] appsrc push failed: " << gst_flow_get_name(flow) << '\n';
        }
    }
}

void H264Encoder::outputLoop() {
    while (running_.load()) {
        GstSample *sample = gst_app_sink_try_pull_sample(appsink_, 100 * GST_MSECOND);
        if (!sample) {
            continue;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map{};
        if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            metrics_.encoder_errors.fetch_add(1);
            gst_sample_unref(sample);
            continue;
        }

        auto payload = std::make_shared<EncodedAccessUnit::Payload>(map.data, map.data + map.size);
        const H264NalSummary summary = inspectH264ByteStream(payload->data(), payload->size());
        auto unit = std::make_shared<EncodedAccessUnit>();
        const std::uint64_t base = base_sensor_timestamp_ns_.load();
        const GstClockTime pts = GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) : 0;
        const GstClockTime dts = GST_BUFFER_DTS_IS_VALID(buffer) ? GST_BUFFER_DTS(buffer) : pts;
        unit->sensor_timestamp_ns = base + pts;
        unit->pts_ns = pts;
        unit->dts_ns = dts;
        unit->duration_ns = GST_BUFFER_DURATION_IS_VALID(buffer)
                                ? GST_BUFFER_DURATION(buffer)
                                : GST_SECOND / config_.fps;
        unit->generation = generation_.load();
        unit->keyframe = summary.has_idr || !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        unit->has_sps = summary.has_sps;
        unit->has_pps = summary.has_pps;
        unit->payload = std::move(payload);

        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);

        metrics_.encoder_access_units.fetch_add(1);
        metrics_.encoder_output_bytes.fetch_add(unit->sizeBytes());
        if (unit->independentlyDecodable()) {
            std::lock_guard<std::mutex> ready_lock(ready_mutex_);
            independent_frame_seen_ = true;
            ready_cv_.notify_all();
        }

        Consumer consumer;
        {
            std::lock_guard<std::mutex> lock(consumer_mutex_);
            consumer = consumer_;
        }
        if (consumer) {
            try {
                consumer(std::move(unit));
            } catch (const std::exception &error) {
                metrics_.encoder_errors.fetch_add(1);
                std::cerr << "[encoder] consumer failed: " << error.what() << '\n';
            }
        }

#ifdef EGGVISION_ENABLE_TEST_HOOKS
        if (!test_failure_injected_.load()) {
            const char *value = std::getenv("EGGVISION_ENCODER_TEST_FAIL_AFTER_AU");
            const std::uint64_t threshold = value ? std::strtoull(value, nullptr, 10) : 0;
            if (threshold > 0 && metrics_.encoder_access_units.load() >= threshold &&
                !test_failure_injected_.exchange(true)) {
                GError *error = g_error_new_literal(g_quark_from_static_string("eggvision-test"),
                                                    1,
                                                    "injected encoder failure");
                GstMessage *message =
                    gst_message_new_error(GST_OBJECT(pipeline_), error, "test hook");
                g_error_free(error);
                gst_element_post_message(pipeline_, message);
            }
        }
#endif
    }
}

void H264Encoder::busLoop() {
    GstBus *bus = gst_element_get_bus(pipeline_);
    while (running_.load()) {
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus,
            100 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!message) {
            continue;
        }
        if (running_.load()) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *error = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                std::cerr << "[encoder] pipeline error; recovery requested: "
                          << (error && error->message ? error->message : "unknown") << '\n';
                g_clear_error(&error);
                g_free(debug);
            } else {
                std::cerr << "[encoder] unexpected EOS; recovery requested\n";
            }
            metrics_.encoder_errors.fetch_add(1);
            recovery_requested_.store(true);
            running_.store(false);
            latest_.close();
            ready_cv_.notify_all();
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
}

void H264Encoder::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const bool was_running = running_.exchange(false);
    latest_.close();
    if (input_thread_.joinable()) {
        input_thread_.join();
    }
    if (appsrc_ && was_running) {
        gst_app_src_end_of_stream(appsrc_);
    }
    if (output_thread_.joinable()) {
        output_thread_.join();
    }
    if (bus_thread_.joinable()) {
        bus_thread_.join();
    }
    {
        std::lock_guard<std::mutex> ready_lock(ready_mutex_);
        ready_cv_.notify_all();
    }
    releasePipeline();
    if (was_running) {
        std::cout << "[encoder] stopped\n";
    }
}

void H264Encoder::releasePipeline() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (appsrc_) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    if (appsink_) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    initialized_.store(false);
}

}  // namespace eggvision
