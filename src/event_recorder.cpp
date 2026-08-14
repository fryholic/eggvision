#include "eggvision/event_recorder.hpp"

#include "eggvision/config.hpp"
#include "eggvision/encoded_ring_buffer.hpp"
#include "eggvision/metrics.hpp"
#include "eggvision/snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <unistd.h>

namespace eggvision {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kWorkerQueueCapacity = 2;

std::uint64_t secondsToNs(double seconds) {
    if (seconds <= 0.0) {
        return 0;
    }
    const long double nanoseconds = static_cast<long double>(seconds) * 1'000'000'000.0L;
    return nanoseconds >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())
               ? std::numeric_limits<std::uint64_t>::max()
               : static_cast<std::uint64_t>(nanoseconds + 0.5L);
}

std::uint64_t saturatingAdd(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

std::tm localTime(std::time_t value) {
    std::tm result{};
    localtime_r(&value, &result);
    return result;
}

std::string formatEventId(const std::chrono::system_clock::time_point &wall_time,
                          std::uint64_t sequence) {
    const std::time_t time = std::chrono::system_clock::to_time_t(wall_time);
    const std::tm local = localTime(time);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  wall_time.time_since_epoch()) %
                              std::chrono::seconds(1);
    char date[32]{};
    char offset[16]{};
    std::strftime(date, sizeof(date), "%Y%m%dT%H%M%S", &local);
    std::strftime(offset, sizeof(offset), "%z", &local);
    std::ostringstream result;
    result << date << '.' << std::setfill('0') << std::setw(3) << milliseconds.count()
           << offset << "_seq" << sequence;
    return result.str();
}

std::string formatDate(const std::chrono::system_clock::time_point &wall_time) {
    const std::time_t time = std::chrono::system_clock::to_time_t(wall_time);
    const std::tm local = localTime(time);
    char value[16]{};
    std::strftime(value, sizeof(value), "%Y-%m-%d", &local);
    return value;
}

std::string formatWallTime(const std::chrono::system_clock::time_point &wall_time) {
    const std::time_t time = std::chrono::system_clock::to_time_t(wall_time);
    const std::tm local = localTime(time);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  wall_time.time_since_epoch()) %
                              std::chrono::seconds(1);
    char date[32]{};
    char offset[16]{};
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local);
    std::strftime(offset, sizeof(offset), "%z", &local);
    std::string formatted_offset = offset;
    if (formatted_offset.size() == 5) {
        formatted_offset.insert(3, ":");
    }
    std::ostringstream result;
    result << date << '.' << std::setfill('0') << std::setw(3) << milliseconds.count()
           << formatted_offset;
    return result.str();
}

std::string jsonEscape(const std::string &value) {
    std::ostringstream result;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result << "\\\""; break;
        case '\\': result << "\\\\"; break;
        case '\b': result << "\\b"; break;
        case '\f': result << "\\f"; break;
        case '\n': result << "\\n"; break;
        case '\r': result << "\\r"; break;
        case '\t': result << "\\t"; break;
        default:
            if (character < 0x20) {
                result << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(character) << std::dec;
            } else {
                result << static_cast<char>(character);
            }
        }
    }
    return result.str();
}

struct EventJob {
    std::string id;
    std::chrono::system_clock::time_point wall_time;
    std::uint64_t trigger_sequence = 0;
    std::uint64_t trigger_sensor_timestamp_ns = 0;
    std::uint64_t requested_start_sensor_timestamp_ns = 0;
    std::uint64_t requested_end_sensor_timestamp_ns = 0;
    std::uint64_t actual_start_sensor_timestamp_ns = 0;
    std::uint64_t actual_end_sensor_timestamp_ns = 0;
    std::uint64_t generation = 0;
    bool pre_roll_complete = false;
    bool post_roll_complete = false;
    MainSnapshot snapshot;
    std::vector<Detection> detections;
    std::vector<EncodedAccessUnitPtr> units;
    std::vector<std::string> errors;
};

struct ActiveEvent : EventJob {};

struct ArtifactResult {
    std::string status = "unavailable";
    std::string path;
    std::uint64_t bytes = 0;
    std::string error;
};

bool writeSnapshot(const MainSnapshot &snapshot,
                   const fs::path &partial_path,
                   const fs::path &final_path,
                   int quality,
                   ArtifactResult &result) {
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    if (std::getenv("EGGVISION_EVENT_TEST_FAIL_SNAPSHOT")) {
        result.status = "failed";
        result.error = "injected snapshot failure";
        return false;
    }
#endif
    if (snapshot.i420.empty() || snapshot.width == 0 || snapshot.height == 0) {
        result.status = "failed";
        result.error = "snapshot pixels are unavailable";
        return false;
    }
    const std::size_t expected =
        static_cast<std::size_t>(snapshot.width) * snapshot.height * 3 / 2;
    if (snapshot.i420.size() != expected) {
        result.status = "failed";
        result.error = "snapshot I420 size does not match its dimensions";
        return false;
    }
    try {
        cv::Mat yuv(static_cast<int>(snapshot.height * 3 / 2),
                    static_cast<int>(snapshot.width),
                    CV_8UC1,
                    const_cast<std::uint8_t *>(snapshot.i420.data()));
        cv::Mat bgr;
        cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
        if (!cv::imwrite(partial_path.string(),
                         bgr,
                         {cv::IMWRITE_JPEG_QUALITY, quality})) {
            result.status = "failed";
            result.error = "OpenCV rejected the JPEG write";
            return false;
        }
        std::error_code error;
        fs::rename(partial_path, final_path, error);
        if (error) {
            result.status = "failed";
            result.error = "snapshot rename failed: " + error.message();
            return false;
        }
        result.status = "complete";
        result.path = final_path.filename().string();
        result.bytes = fs::file_size(final_path, error);
        if (error) {
            result.bytes = 0;
        }
        return true;
    } catch (const cv::Exception &error) {
        result.status = "failed";
        result.error = std::string("OpenCV JPEG failure: ") + error.what();
        return false;
    }
}

bool muxVideo(const EventJob &job,
              const AppConfig &config,
              const fs::path &partial_path,
              const fs::path &final_path,
              ArtifactResult &result) {
    if (job.units.empty()) {
        result.status = "unavailable";
        result.error = "no independently decodable H.264 pre-roll was available";
        return false;
    }
    if (!job.units.front()->independentlyDecodable()) {
        result.status = "failed";
        result.error = "event video does not begin with IDR/SPS/PPS";
        return false;
    }
#ifdef EGGVISION_ENABLE_TEST_HOOKS
    if (std::getenv("EGGVISION_EVENT_TEST_FAIL_MUX")) {
        result.status = "failed";
        result.error = "injected mux failure";
        return false;
    }
#endif
    for (const auto &unit : job.units) {
        if (!unit || !unit->payload || unit->payload->empty() ||
            unit->generation != job.generation) {
            result.status = "failed";
            result.error = "event video contains an invalid or mixed-generation access unit";
            return false;
        }
    }

    const bool mp4 = config.event_container == "mp4";
    const std::string muxer = mp4 ? "mp4mux" : "matroskamux";
    const std::string pipeline_description =
        "appsrc name=event_source is-live=false format=time block=true "
        "! video/x-h264,stream-format=byte-stream,alignment=au "
        "! h264parse config-interval=-1 ! " +
        muxer + " ! filesink name=event_sink sync=false";
    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_description.c_str(), &parse_error);
    if (!pipeline) {
        result.status = "failed";
        result.error = std::string("event mux pipeline creation failed: ") +
                       (parse_error && parse_error->message ? parse_error->message : "unknown");
        g_clear_error(&parse_error);
        return false;
    }
    if (parse_error) {
        g_clear_error(&parse_error);
    }

    GstElement *source_element = gst_bin_get_by_name(GST_BIN(pipeline), "event_source");
    GstElement *sink_element = gst_bin_get_by_name(GST_BIN(pipeline), "event_sink");
    auto cleanup = [&] {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (source_element) {
            gst_object_unref(source_element);
        }
        if (sink_element) {
            gst_object_unref(sink_element);
        }
        gst_object_unref(pipeline);
    };
    if (!source_element || !sink_element || !GST_IS_APP_SRC(source_element)) {
        result.status = "failed";
        result.error = "event mux pipeline is missing appsrc or filesink";
        cleanup();
        return false;
    }
    auto *source = GST_APP_SRC(source_element);
    g_object_set(sink_element, "location", partial_path.string().c_str(), nullptr);
    GstCaps *caps = gst_caps_new_simple("video/x-h264",
                                        "stream-format", G_TYPE_STRING, "byte-stream",
                                        "alignment", G_TYPE_STRING, "au",
                                        "width", G_TYPE_INT, static_cast<int>(config.main_width),
                                        "height", G_TYPE_INT, static_cast<int>(config.main_height),
                                        "framerate", GST_TYPE_FRACTION,
                                        static_cast<int>(config.fps), 1,
                                        nullptr);
    gst_app_src_set_caps(source, caps);
    gst_caps_unref(caps);
    gst_app_src_set_stream_type(source, GST_APP_STREAM_TYPE_STREAM);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        result.status = "failed";
        result.error = "event mux pipeline failed to enter PLAYING";
        cleanup();
        return false;
    }

    const std::uint64_t base_timestamp = job.units.front()->sensor_timestamp_ns;
    bool pushed = true;
    for (const auto &unit : job.units) {
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, unit->payload->size(), nullptr);
        if (!buffer ||
            gst_buffer_fill(buffer, 0, unit->payload->data(), unit->payload->size()) !=
                unit->payload->size()) {
            if (buffer) {
                gst_buffer_unref(buffer);
            }
            result.error = "event mux could not allocate an H.264 buffer";
            pushed = false;
            break;
        }
        const std::uint64_t local_timestamp =
            unit->sensor_timestamp_ns >= base_timestamp
                ? unit->sensor_timestamp_ns - base_timestamp
                : 0;
        GST_BUFFER_PTS(buffer) = local_timestamp;
        GST_BUFFER_DTS(buffer) = local_timestamp;
        GST_BUFFER_DURATION(buffer) = unit->duration_ns;
        if (!unit->keyframe) {
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        }
        const GstFlowReturn flow = gst_app_src_push_buffer(source, buffer);
        if (flow != GST_FLOW_OK) {
            result.error = std::string("event mux appsrc push failed: ") +
                           gst_flow_get_name(flow);
            pushed = false;
            break;
        }
    }

    bool finalized = false;
    if (pushed && gst_app_src_end_of_stream(source) == GST_FLOW_OK) {
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus,
            15 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            finalized = true;
        } else if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            result.error = std::string("event mux failed: ") +
                           (error && error->message ? error->message : "unknown");
            g_clear_error(&error);
            g_free(debug);
        } else {
            result.error = "event mux did not finalize within 15 seconds";
        }
        if (message) {
            gst_message_unref(message);
        }
        gst_object_unref(bus);
    } else if (result.error.empty()) {
        result.error = "event mux could not send EOS";
    }
    cleanup();

    if (!finalized) {
        result.status = "failed";
        return false;
    }
    std::error_code error;
    fs::rename(partial_path, final_path, error);
    if (error) {
        result.status = "failed";
        result.error = "video rename failed: " + error.message();
        return false;
    }
    result.status = "complete";
    result.path = final_path.filename().string();
    result.bytes = fs::file_size(final_path, error);
    if (error) {
        result.bytes = 0;
    }
    return true;
}

bool writeMetadata(const EventJob &job,
                   const AppConfig &config,
                   const ArtifactResult &snapshot,
                   const ArtifactResult &video,
                   const std::vector<std::string> &errors,
                   const fs::path &path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"event_id\": \"" << jsonEscape(job.id) << "\",\n"
           << "  \"trigger_wall_time\": \"" << jsonEscape(formatWallTime(job.wall_time))
           << "\",\n"
           << "  \"trigger_sequence\": " << job.trigger_sequence << ",\n"
           << "  \"trigger_sensor_timestamp_ns\": "
           << job.trigger_sensor_timestamp_ns << ",\n"
           << "  \"requested_pre_roll_ms\": "
           << static_cast<std::uint64_t>(config.event_pre_seconds * 1000.0 + 0.5) << ",\n"
           << "  \"requested_post_roll_ms\": "
           << static_cast<std::uint64_t>(config.event_post_seconds * 1000.0 + 0.5) << ",\n"
           << "  \"requested_start_sensor_timestamp_ns\": "
           << job.requested_start_sensor_timestamp_ns << ",\n"
           << "  \"requested_end_sensor_timestamp_ns\": "
           << job.requested_end_sensor_timestamp_ns << ",\n"
           << "  \"actual_start_sensor_timestamp_ns\": "
           << job.actual_start_sensor_timestamp_ns << ",\n"
           << "  \"actual_end_sensor_timestamp_ns\": "
           << job.actual_end_sensor_timestamp_ns << ",\n"
           << "  \"pre_roll_complete\": "
           << (job.pre_roll_complete ? "true" : "false") << ",\n"
           << "  \"post_roll_complete\": "
           << (job.post_roll_complete ? "true" : "false") << ",\n"
           << "  \"video\": {\n"
           << "    \"status\": \"" << video.status << "\",\n"
           << "    \"path\": "
           << (video.path.empty() ? "null" : "\"" + jsonEscape(video.path) + "\"") << ",\n"
           << "    \"codec\": \"h264\",\n"
           << "    \"profile\": \"high\",\n"
           << "    \"level\": \"4\",\n"
           << "    \"container\": \"" << jsonEscape(config.event_container) << "\",\n"
           << "    \"width\": " << config.main_width << ",\n"
           << "    \"height\": " << config.main_height << ",\n"
           << "    \"nominal_fps\": " << config.fps << ",\n"
           << "    \"access_units\": " << job.units.size() << ",\n"
           << "    \"bytes\": " << video.bytes << "\n"
           << "  },\n"
           << "  \"snapshot\": {\n"
           << "    \"status\": \"" << snapshot.status << "\",\n"
           << "    \"path\": "
           << (snapshot.path.empty() ? "null" : "\"" + jsonEscape(snapshot.path) + "\"")
           << ",\n"
           << "    \"width\": " << job.snapshot.width << ",\n"
           << "    \"height\": " << job.snapshot.height << ",\n"
           << "    \"sequence\": " << job.snapshot.sequence << ",\n"
           << "    \"sensor_timestamp_ns\": "
           << job.snapshot.sensor_timestamp_ns << ",\n"
           << "    \"jpeg_quality\": " << config.event_jpeg_quality << "\n"
           << "  },\n"
           << "  \"detections\": [";
    for (std::size_t index = 0; index < job.detections.size(); ++index) {
        const Detection &detection = job.detections[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"class_id\": " << detection.class_id
               << ", \"confidence\": " << detection.confidence
               << ", \"lores_box\": {\"x\": " << detection.box.x
               << ", \"y\": " << detection.box.y
               << ", \"width\": " << detection.box.width
               << ", \"height\": " << detection.box.height << "}}";
    }
    output << (job.detections.empty() ? "]" : "\n  ]") << ",\n"
           << "  \"encoder_generation\": " << job.generation << ",\n"
           << "  \"errors\": [";
    for (std::size_t index = 0; index < errors.size(); ++index) {
        output << (index == 0 ? "" : ", ") << '"' << jsonEscape(errors[index]) << '"';
    }
    output << "]\n}\n";
    output.flush();
    return output.good();
}

}  // namespace

EventGate::EventGate(std::uint64_t cooldown_ns) : cooldown_ns_(cooldown_ns) {}

bool EventGate::tryBegin(std::uint64_t trigger_sensor_timestamp_ns) {
    if (collecting_ || trigger_sensor_timestamp_ns < cooldown_until_ns_) {
        return false;
    }
    collecting_ = true;
    return true;
}

void EventGate::complete(std::uint64_t actual_end_sensor_timestamp_ns) {
    collecting_ = false;
    cooldown_until_ns_ = saturatingAdd(actual_end_sensor_timestamp_ns, cooldown_ns_);
}

EventState EventGate::stateAt(std::uint64_t sensor_timestamp_ns) const {
    if (collecting_) {
        return EventState::CollectingPostRoll;
    }
    return sensor_timestamp_ns < cooldown_until_ns_ ? EventState::Cooldown : EventState::Idle;
}

struct EventRecorder::Impl {
    Impl(const AppConfig &values, EncodedRingBuffer &buffer, Metrics &counters)
        : config(values), history(buffer), metrics(counters),
          gate(secondsToNs(values.event_cooldown_seconds)), root(values.events_dir) {}

    const AppConfig config;
    EncodedRingBuffer &history;
    Metrics &metrics;
    EventGate gate;
    fs::path root;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::optional<ActiveEvent> active;
    std::deque<EventJob> jobs;
    std::thread worker;
    bool initialized = false;
    bool running = false;
    bool accepting = false;
    std::uint64_t latest_sensor_timestamp_ns = 0;

    bool storageAvailable(bool initializing) {
#ifdef EGGVISION_ENABLE_TEST_HOOKS
        if (!initializing && std::getenv("EGGVISION_EVENT_TEST_REJECT_STORAGE")) {
            metrics.event_disk_space_rejections.fetch_add(1);
            std::cerr << "{\"type\":\"event_storage_rejected\","
                         "\"error\":\"injected runtime storage rejection\"}\n";
            return false;
        }
#endif
        std::error_code error;
        const fs::space_info space = fs::space(root, error);
        if (error || space.available < config.event_min_free_bytes) {
            metrics.event_disk_space_rejections.fetch_add(1);
            std::cerr << "{\"type\":\"event_storage_rejected\",\"available_bytes\":"
                      << (error ? 0 : space.available)
                      << ",\"minimum_bytes\":" << config.event_min_free_bytes
                      << ",\"error\":\"" << jsonEscape(error.message()) << "\"}\n";
            return false;
        }
        return true;
    }

    void finishLocked(bool post_complete, const std::string &error = {}) {
        if (!active) {
            return;
        }
        active->post_roll_complete = post_complete;
        if (!error.empty()) {
            active->errors.push_back(error);
        }
        if (!post_complete) {
            metrics.events_partial_postroll.fetch_add(1);
        }
        if (!active->units.empty()) {
            active->actual_start_sensor_timestamp_ns =
                active->units.front()->sensor_timestamp_ns;
            active->actual_end_sensor_timestamp_ns =
                active->units.back()->sensor_timestamp_ns;
        } else {
            active->actual_end_sensor_timestamp_ns = active->trigger_sensor_timestamp_ns;
        }
        const std::uint64_t cooldown_base = active->actual_end_sensor_timestamp_ns;
        gate.complete(cooldown_base);
        if (jobs.size() >= kWorkerQueueCapacity) {
            metrics.events_failed.fetch_add(1);
            std::cerr << "{\"type\":\"event_failed\",\"event_id\":\""
                      << active->id << "\",\"error\":\"event worker queue is full\"}\n";
            active.reset();
            return;
        }
        jobs.push_back(std::move(*active));
        active.reset();
        cv.notify_one();
    }

    void workerLoop() {
        for (;;) {
            EventJob job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return !running || !jobs.empty(); });
                if (jobs.empty() && !running) {
                    break;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            writeEvent(job);
        }
    }

    void writeEvent(EventJob &job) {
        std::error_code error;
        const fs::path date_dir = root / formatDate(job.wall_time);
        fs::create_directories(date_dir, error);
        const fs::path partial_dir = date_dir / ("." + job.id + ".partial");
        const fs::path final_dir = date_dir / job.id;
        if (error || fs::exists(partial_dir) || fs::exists(final_dir) ||
            !fs::create_directory(partial_dir, error)) {
            metrics.events_failed.fetch_add(1);
            std::cerr << "{\"type\":\"event_failed\",\"event_id\":\"" << job.id
                      << "\",\"error\":\"event directory creation failed: "
                      << jsonEscape(error.message()) << "\"}\n";
            return;
        }

        std::cout << "{\"type\":\"event_mux_started\",\"event_id\":\"" << job.id
                  << "\",\"access_units\":" << job.units.size() << "}\n";
        ArtifactResult snapshot;
        const bool snapshot_ok = writeSnapshot(job.snapshot,
                                               partial_dir / "snapshot.partial.jpg",
                                               partial_dir / "snapshot.jpg",
                                               config.event_jpeg_quality,
                                               snapshot);
        if (!snapshot_ok) {
            metrics.event_snapshot_errors.fetch_add(1);
            if (!snapshot.error.empty()) {
                job.errors.push_back(snapshot.error);
            }
        } else {
            metrics.event_snapshot_bytes.fetch_add(snapshot.bytes);
        }

        ArtifactResult video;
        const std::string extension = config.event_container == "mp4" ? ".mp4" : ".mkv";
        const bool video_ok = muxVideo(job,
                                       config,
                                       partial_dir / ("clip.partial" + extension),
                                       partial_dir / ("clip" + extension),
                                       video);
        const bool fatal_mux_failure = !video_ok && video.status == "failed";
        if (!video_ok && video.status == "failed") {
            metrics.event_mux_errors.fetch_add(1);
            if (!video.error.empty()) {
                job.errors.push_back(video.error);
            }
        } else if (video_ok) {
            metrics.event_video_bytes.fetch_add(video.bytes);
        }

        const fs::path metadata_partial = partial_dir / "metadata.partial.json";
        if (!writeMetadata(job, config, snapshot, video, job.errors, metadata_partial)) {
            metrics.events_failed.fetch_add(1);
            std::cerr << "{\"type\":\"event_failed\",\"event_id\":\"" << job.id
                      << "\",\"error\":\"metadata write failed\"}\n";
            return;
        }

        if (fatal_mux_failure || (!snapshot_ok && !video_ok)) {
            metrics.events_failed.fetch_add(1);
            std::cerr << "{\"type\":\"event_failed\",\"event_id\":\"" << job.id
                      << "\",\"partial_dir\":\"" << jsonEscape(partial_dir.string())
                      << "\",\"error\":\"artifact finalize failed\"}\n";
            return;
        }

        fs::rename(metadata_partial, partial_dir / "metadata.json", error);
        if (!error) {
            fs::rename(partial_dir, final_dir, error);
        }
        if (error) {
            metrics.events_failed.fetch_add(1);
            std::cerr << "{\"type\":\"event_failed\",\"event_id\":\"" << job.id
                      << "\",\"error\":\"atomic publish failed: "
                      << jsonEscape(error.message()) << "\"}\n";
            return;
        }
        metrics.events_completed.fetch_add(1);
        std::cout << "{\"type\":\"event_completed\",\"event_id\":\"" << job.id
                  << "\",\"video_bytes\":" << video.bytes
                  << ",\"snapshot_bytes\":" << snapshot.bytes
                  << ",\"pre_roll_complete\":"
                  << (job.pre_roll_complete ? "true" : "false")
                  << ",\"post_roll_complete\":"
                  << (job.post_roll_complete ? "true" : "false") << "}\n";
    }
};

EventRecorder::EventRecorder(const AppConfig &config,
                             EncodedRingBuffer &history,
                             Metrics &metrics)
    : impl_(std::make_unique<Impl>(config, history, metrics)) {
    gst_init(nullptr, nullptr);
}

EventRecorder::~EventRecorder() {
    stop();
}

bool EventRecorder::initialize() {
    if (!impl_->config.event_recording_enabled) {
        impl_->initialized = true;
        return true;
    }
    std::error_code error;
    fs::create_directories(impl_->root, error);
    if (error) {
        std::cerr << "[event] cannot create events directory: " << error.message() << '\n';
        return false;
    }
    const fs::path probe =
        impl_->root / (".eggvision-write-probe-" + std::to_string(static_cast<long>(getpid())));
    {
        std::ofstream output(probe, std::ios::binary | std::ios::trunc);
        output << "ok\n";
        if (!output.good()) {
            std::cerr << "[event] events directory is not writable: " << impl_->root << '\n';
            return false;
        }
    }
    fs::remove(probe, error);
    if (!impl_->storageAvailable(true)) {
        std::cerr << "[event] storage reserve check failed during initialization\n";
        return false;
    }
    for (const auto &entry : fs::recursive_directory_iterator(impl_->root, error)) {
        if (error) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && name.size() > 9 && name.front() == '.' &&
            name.rfind(".partial") == name.size() - 8) {
            std::cerr << "[event] stale partial directory retained: " << entry.path() << '\n';
        }
    }
    impl_->initialized = true;
    std::cout << "[event] initialized root=" << impl_->root
              << " pre=" << impl_->config.event_pre_seconds
              << " post=" << impl_->config.event_post_seconds
              << " cooldown=" << impl_->config.event_cooldown_seconds << '\n';
    return true;
}

bool EventRecorder::start() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        return false;
    }
    if (!impl_->config.event_recording_enabled || impl_->running) {
        impl_->accepting = impl_->config.event_recording_enabled;
        return true;
    }
    impl_->running = true;
    impl_->accepting = true;
    try {
        impl_->worker = std::thread(&Impl::workerLoop, impl_.get());
    } catch (const std::system_error &error) {
        impl_->running = false;
        impl_->accepting = false;
        std::cerr << "[event] failed to start worker: " << error.what() << '\n';
        return false;
    }
    return true;
}

bool EventRecorder::trigger(const std::shared_ptr<FrameLease> &frame,
                            const std::vector<Detection> &detections) {
    if (!impl_->config.event_recording_enabled || !frame || detections.empty()) {
        return false;
    }
    if (!impl_->storageAvailable(false)) {
        impl_->metrics.events_suppressed.fetch_add(1);
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::uint64_t trigger_timestamp = frame->sensorTimestampNs();
    impl_->latest_sensor_timestamp_ns =
        std::max(impl_->latest_sensor_timestamp_ns, trigger_timestamp);
    if (!impl_->accepting || !impl_->gate.tryBegin(trigger_timestamp)) {
        impl_->metrics.events_suppressed.fetch_add(1);
        std::cout << "{\"type\":\"event_suppressed\",\"sequence\":"
                  << frame->sequence() << ",\"sensor_timestamp_ns\":"
                  << trigger_timestamp << "}\n";
        return false;
    }

    ActiveEvent event;
    event.wall_time = std::chrono::system_clock::now();
    event.id = formatEventId(event.wall_time, frame->sequence());
    event.trigger_sequence = frame->sequence();
    event.trigger_sensor_timestamp_ns = trigger_timestamp;
    const std::uint64_t pre_ns = secondsToNs(impl_->config.event_pre_seconds);
    const std::uint64_t post_ns = secondsToNs(impl_->config.event_post_seconds);
    event.requested_start_sensor_timestamp_ns =
        trigger_timestamp > pre_ns ? trigger_timestamp - pre_ns : 0;
    event.requested_end_sensor_timestamp_ns = saturatingAdd(trigger_timestamp, post_ns);
    event.detections = detections;

    std::string snapshot_error;
    if (!stageMainSnapshot(*frame, event.snapshot, snapshot_error)) {
        event.errors.push_back(snapshot_error);
    }

    const EncodedRingSelection selection = impl_->history.selectPreRoll(trigger_timestamp, pre_ns);
    event.generation = selection.generation;
    event.pre_roll_complete = selection.pre_roll_complete;
    for (const auto &unit : selection.units) {
        event.units.push_back(unit);
        if (unit->sensor_timestamp_ns >= event.requested_end_sensor_timestamp_ns) {
            event.post_roll_complete = true;
            break;
        }
    }
    if (!event.pre_roll_complete) {
        impl_->metrics.events_partial_preroll.fetch_add(1);
    }
    impl_->metrics.events_triggered.fetch_add(1);
    std::cout << "{\"type\":\"event_triggered\",\"event_id\":\"" << event.id
              << "\",\"sequence\":" << event.trigger_sequence
              << ",\"sensor_timestamp_ns\":" << trigger_timestamp
              << ",\"generation\":" << event.generation
              << ",\"pre_roll_complete\":"
              << (event.pre_roll_complete ? "true" : "false") << "}\n";
    impl_->active = std::move(event);

    if (!selection.has_independent_start || impl_->active->units.empty()) {
        impl_->active->errors.push_back(
            "no independently decodable H.264 pre-roll was available");
        impl_->finishLocked(false);
    } else if (impl_->active->post_roll_complete) {
        impl_->finishLocked(true);
    } else {
        std::cout << "{\"type\":\"event_collecting\",\"event_id\":\""
                  << impl_->active->id << "\",\"requested_end_sensor_timestamp_ns\":"
                  << impl_->active->requested_end_sensor_timestamp_ns << "}\n";
    }
    return true;
}

void EventRecorder::observeEncoded(const EncodedAccessUnitPtr &unit) {
    if (!impl_->config.event_recording_enabled || !unit) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->latest_sensor_timestamp_ns =
        std::max(impl_->latest_sensor_timestamp_ns, unit->sensor_timestamp_ns);
    if (!impl_->active) {
        return;
    }
    if (unit->generation != impl_->active->generation) {
        impl_->finishLocked(false, "encoder generation changed during post-roll");
        return;
    }
    if (!impl_->active->units.empty() &&
        unit->sensor_timestamp_ns <= impl_->active->units.back()->sensor_timestamp_ns) {
        return;
    }
    impl_->active->units.push_back(unit);
    if (unit->sensor_timestamp_ns >= impl_->active->requested_end_sensor_timestamp_ns) {
        impl_->finishLocked(true);
    }
}

void EventRecorder::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->accepting = false;
        if (impl_->active) {
            impl_->finishLocked(false, "shutdown interrupted event post-roll");
        }
        if (!impl_->running) {
            return;
        }
        impl_->running = false;
        impl_->cv.notify_all();
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

EventState EventRecorder::state() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->gate.stateAt(impl_->latest_sensor_timestamp_ns);
}

}  // namespace eggvision
