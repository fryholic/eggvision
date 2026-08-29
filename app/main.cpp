#include "eggvision/camera_capture.hpp"
#include "eggvision/config.hpp"
#include "eggvision/encoded_ring_buffer.hpp"
#include "eggvision/event_recorder.hpp"
#include "eggvision/h264_encoder.hpp"
#include "eggvision/inference.hpp"
#include "eggvision/metrics.hpp"
#include "eggvision/rtsp_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> exit_requested{false};

void signalHandler(int) {
    exit_requested.store(true);
}

void usage(const char *program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --model PATH          OpenVINO YOLOv5n XML (default models/yolov5n.xml)\n"
        << "  --port PORT           RTSP port (default 8554)\n"
        << "  --mount PATH          RTSP mount (default /stream)\n"
        << "  --max-rtsp-sessions N Maximum concurrent/pending RTSP sessions (default 32)\n"
        << "  --bitrate BPS         H.264 bitrate (default 4000000)\n"
        << "  --gop N               H.264 GOP length (default 12)\n"
        << "  --confidence VALUE    person confidence threshold (default 0.30)\n"
        << "  --nms VALUE           NMS IoU threshold (default 0.45)\n"
        << "  --inference-threads N OpenVINO CPU threads (default 2)\n"
        << "  --events-dir PATH     event artifact root (default /var/lib/eggvision/events)\n"
        << "  --event-pre-seconds N event pre-roll duration (default 1.5)\n"
        << "  --event-post-seconds N event post-roll duration (default 1.5)\n"
        << "  --event-cooldown-seconds N suppression after an event (default 10)\n"
        << "  --event-ring-seconds N encoded history duration (default 4)\n"
        << "  --event-ring-max-bytes N encoded history byte cap (default 8388608)\n"
        << "  --event-min-free-bytes N storage reserve (default 1073741824)\n"
        << "  --event-jpeg-quality N JPEG quality from 1 to 100 (default 90)\n"
        << "  --event-container mp4|mkv container format (default mp4)\n"
        << "  --no-event-recording disable detection event artifacts\n"
        << "  --duration SECONDS    stop automatically; 0 means run until signal\n"
        << "  --no-inference        stream without running OpenVINO\n"
        << "  --help                show this help\n";
}

eggvision::AppConfig parseArguments(int argc, char **argv) {
    eggvision::AppConfig config;
    auto value = [&](int &index) -> std::string {
        if (++index >= argc) {
            throw std::runtime_error(std::string("missing value after ") + argv[index - 1]);
        }
        return argv[index];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--model") {
            config.model_path = value(i);
        } else if (argument == "--port") {
            config.rtsp_port = value(i);
        } else if (argument == "--mount") {
            config.rtsp_mount = value(i);
        } else if (argument == "--max-rtsp-sessions") {
            config.rtsp_max_sessions = static_cast<unsigned>(std::stoul(value(i)));
        } else if (argument == "--bitrate") {
            config.bitrate = static_cast<unsigned>(std::stoul(value(i)));
        } else if (argument == "--gop") {
            config.gop = static_cast<unsigned>(std::stoul(value(i)));
        } else if (argument == "--confidence") {
            config.confidence_threshold = std::stof(value(i));
        } else if (argument == "--nms") {
            config.nms_threshold = std::stof(value(i));
        } else if (argument == "--inference-threads") {
            config.inference_threads = static_cast<unsigned>(std::stoul(value(i)));
        } else if (argument == "--events-dir") {
            config.events_dir = value(i);
        } else if (argument == "--event-pre-seconds") {
            config.event_pre_seconds = std::stod(value(i));
        } else if (argument == "--event-post-seconds") {
            config.event_post_seconds = std::stod(value(i));
        } else if (argument == "--event-cooldown-seconds") {
            config.event_cooldown_seconds = std::stod(value(i));
        } else if (argument == "--event-ring-seconds") {
            config.event_ring_seconds = std::stod(value(i));
        } else if (argument == "--event-ring-max-bytes") {
            config.event_ring_max_bytes = std::stoull(value(i));
        } else if (argument == "--event-min-free-bytes") {
            config.event_min_free_bytes = std::stoull(value(i));
        } else if (argument == "--event-jpeg-quality") {
            config.event_jpeg_quality = std::stoi(value(i));
        } else if (argument == "--event-container") {
            config.event_container = value(i);
        } else if (argument == "--duration") {
            config.duration_seconds = static_cast<unsigned>(std::stoul(value(i)));
        } else if (argument == "--no-inference") {
            config.inference_enabled = false;
        } else if (argument == "--no-event-recording") {
            config.event_recording_enabled = false;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    if (config.rtsp_mount.empty() || config.rtsp_mount.front() != '/') {
        throw std::runtime_error("--mount must begin with '/'");
    }
    if (config.confidence_threshold < 0.0F || config.confidence_threshold > 1.0F ||
        config.nms_threshold < 0.0F || config.nms_threshold > 1.0F) {
        throw std::runtime_error("confidence and NMS thresholds must be in [0,1]");
    }
    if (config.inference_threads == 0) {
        throw std::runtime_error("--inference-threads must be at least 1");
    }
    if (config.rtsp_max_sessions == 0) {
        throw std::runtime_error("--max-rtsp-sessions must be at least 1");
    }
    if (config.bitrate == 0 || config.gop == 0) {
        throw std::runtime_error("--bitrate and --gop must be at least 1");
    }
    if (config.event_recording_enabled) {
        if (config.events_dir.empty()) {
            throw std::runtime_error("--events-dir must not be empty");
        }
        if (config.event_pre_seconds <= 0.0 || config.event_post_seconds <= 0.0 ||
            config.event_cooldown_seconds < 0.0 || config.event_ring_seconds <= 0.0) {
            throw std::runtime_error("event durations must be positive; cooldown may be zero");
        }
        const double minimum_ring = config.event_pre_seconds +
                                    static_cast<double>(config.gop) / config.fps + 0.5;
        if (config.event_ring_seconds < minimum_ring) {
            throw std::runtime_error(
                "--event-ring-seconds must cover pre-roll, one GOP, and 0.5s safety");
        }
        if (config.event_ring_max_bytes < 1024ULL * 1024ULL) {
            throw std::runtime_error("--event-ring-max-bytes must be at least 1048576");
        }
        if (config.event_jpeg_quality < 1 || config.event_jpeg_quality > 100) {
            throw std::runtime_error("--event-jpeg-quality must be in [1,100]");
        }
        if (config.event_container != "mp4" && config.event_container != "mkv") {
            throw std::runtime_error("--event-container must be mp4 or mkv");
        }
    }
    return config;
}

void printConfiguration(const eggvision::AppConfig &config) {
    std::cout << "[config] main=" << config.main_width << 'x' << config.main_height
              << " lores=" << config.lores_width << 'x' << config.lores_height
              << " fps=" << config.fps << " buffers=" << config.buffer_count
              << " rtsp=" << config.rtsp_address << ':' << config.rtsp_port << config.rtsp_mount
              << " max_rtsp_sessions=" << config.rtsp_max_sessions
              << " bitrate=" << config.bitrate << " gop=" << config.gop
              << " inference=" << (config.inference_enabled ? "on" : "off")
              << " model=" << config.model_path
              << " confidence=" << config.confidence_threshold
              << " nms=" << config.nms_threshold
              << " inference_threads=" << config.inference_threads << '\n';
    std::cout << "[config] event_recording="
              << (config.event_recording_enabled ? "on" : "off")
              << " events_dir=" << config.events_dir
              << " pre=" << config.event_pre_seconds
              << " post=" << config.event_post_seconds
              << " cooldown=" << config.event_cooldown_seconds
              << " ring_seconds=" << config.event_ring_seconds
              << " ring_max_bytes=" << config.event_ring_max_bytes
              << " min_free_bytes=" << config.event_min_free_bytes
              << " jpeg_quality=" << config.event_jpeg_quality
              << " container=" << config.event_container << '\n';
}

}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        const eggvision::AppConfig config = parseArguments(argc, argv);
        printConfiguration(config);
        eggvision::Metrics metrics;
        eggvision::RtspServer rtsp(config, metrics);
        eggvision::H264Encoder encoder(config, metrics);
        const std::uint64_t ring_retention_ns = static_cast<std::uint64_t>(
            config.event_ring_seconds * 1'000'000'000.0 + 0.5);
        eggvision::EncodedRingBuffer encoded_history(ring_retention_ns,
                                                      config.event_ring_max_bytes);
        eggvision::EventRecorder event_recorder(config, encoded_history, metrics);
        eggvision::InferenceWorker inference(config, metrics);
        eggvision::CameraCapture camera(config, metrics);

        if (!event_recorder.initialize() || !inference.initialize() ||
            !encoder.initialize() || !camera.initialize()) {
            return 2;
        }
        encoder.setConsumer([&](eggvision::EncodedAccessUnitPtr unit) {
            const eggvision::EncodedRingPushResult result = encoded_history.push(unit);
            if (result == eggvision::EncodedRingPushResult::ResetForGeneration ||
                result == eggvision::EncodedRingPushResult::ResetForTimestampRegression) {
                std::cout << "{\"type\":\"encoded_ring_reset\",\"generation\":"
                          << unit->generation << ",\"reason\":\""
                          << (result == eggvision::EncodedRingPushResult::ResetForGeneration
                                  ? "generation"
                                  : "timestamp_regression")
                          << "\"}\n";
            }
            event_recorder.observeEncoded(unit);
            rtsp.submit(std::move(unit));
        });

#ifdef EGGVISION_ENABLE_TEST_HOOKS
        std::uint64_t test_trigger_delay_ns = 0;
        if (const char *value = std::getenv("EGGVISION_EVENT_TEST_TRIGGER_AFTER_MS")) {
            test_trigger_delay_ns = std::stoull(value) * 1'000'000ULL;
            std::cout << "[test-hook] deterministic event trigger after " << value << " ms\n";
        }
        std::atomic<std::uint64_t> test_first_sensor_timestamp_ns{0};
        std::atomic<bool> test_event_triggered{false};
#endif
        camera.setMainConsumer([&](std::shared_ptr<eggvision::FrameLease> frame) {
#ifdef EGGVISION_ENABLE_TEST_HOOKS
            if (test_trigger_delay_ns > 0 && !test_event_triggered.load()) {
                std::uint64_t first = test_first_sensor_timestamp_ns.load();
                if (first == 0) {
                    test_first_sensor_timestamp_ns.compare_exchange_strong(
                        first, frame->sensorTimestampNs());
                    first = test_first_sensor_timestamp_ns.load();
                }
                if (frame->sensorTimestampNs() >= first + test_trigger_delay_ns) {
                    if (!test_event_triggered.exchange(true)) {
                        eggvision::Detection detection;
                        detection.class_id = 0;
                        detection.confidence = 1.0F;
                        detection.box = {0.0F,
                                         0.0F,
                                         static_cast<float>(config.lores_width),
                                         static_cast<float>(config.lores_height)};
                        event_recorder.trigger(frame, {detection});
                    }
                }
            }
#endif
            encoder.submit(std::move(frame));
        });
        if (config.inference_enabled) {
            if (config.event_recording_enabled) {
                inference.setDetectionConsumer(
                    [&event_recorder](const std::shared_ptr<eggvision::FrameLease> &frame,
                                      const std::vector<eggvision::Detection> &detections) {
                        event_recorder.trigger(frame, detections);
                    });
            }
            camera.setInferenceConsumer([&inference](std::shared_ptr<eggvision::FrameLease> frame) {
                inference.submit(std::move(frame));
            });
        }

        // Do not expose the RTSP listener until the camera is producing frames;
        // otherwise an eager client can make the first DESCRIBE fail with 503.
        if (!event_recorder.start() || !encoder.start() || !inference.start() || !camera.start() ||
            !encoder.waitForIndependentFrame(std::chrono::seconds(5)) || !rtsp.start()) {
            camera.stop();
            inference.stop();
            encoder.stop();
            event_recorder.stop();
            rtsp.stop();
            std::cout << "[app] startup failed outstanding="
                      << metrics.outstanding_leases.load()
                      << " encoder_errors=" << metrics.encoder_errors.load()
                      << " capture_errors=" << metrics.capture_errors.load()
                      << " rtsp_errors=" << metrics.rtsp_errors.load()
                      << " rtsp_recoveries=" << metrics.rtsp_recoveries.load()
                      << " rtsp_recovery_failures="
                      << metrics.rtsp_recovery_failures.load() << '\n';
            return 3;
        }

        const auto started = std::chrono::steady_clock::now();
        auto last_report = started;
        std::uint64_t last_capture = 0;
        std::uint64_t last_rtsp = 0;
        std::uint64_t last_inference = 0;
        bool fatal_encoder_failure = false;
        while (!exit_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (encoder.recoveryRequested()) {
                std::cout << "{\"type\":\"encoder_recovery_started\"}\n";
                encoder.stop();
                encoded_history.clear();
                if (!encoder.initialize() || !encoder.start() ||
                    !encoder.waitForIndependentFrame(std::chrono::seconds(5))) {
                    metrics.encoder_recovery_failures.fetch_add(1);
                    std::cerr << "{\"type\":\"encoder_recovery_failed\"}\n";
                    fatal_encoder_failure = true;
                    break;
                }
                metrics.encoder_recoveries.fetch_add(1);
                std::cout << "{\"type\":\"encoder_recovered\"}\n";
            }
            const auto now = std::chrono::steady_clock::now();
            if (config.duration_seconds > 0 &&
                now - started >= std::chrono::seconds(config.duration_seconds)) {
                break;
            }
            if (now - last_report < std::chrono::seconds(config.metrics_interval_seconds)) {
                continue;
            }
            const double seconds = std::chrono::duration<double>(now - last_report).count();
            const std::uint64_t captured = metrics.captured.load();
            const std::uint64_t pushed = metrics.rtsp_pushed.load();
            const std::uint64_t inferred = metrics.inference_processed.load();
            const eggvision::EncodedRingStats ring = encoded_history.stats();
            const double average_inference_ms = inferred == 0
                                                    ? 0.0
                                                    : metrics.inference_total_us.load() / 1000.0 / inferred;
            const double average_input_ms = inferred == 0
                                                ? 0.0
                                                : metrics.inference_input_total_us.load() / 1000.0 /
                                                      inferred;
            std::cout << std::fixed << std::setprecision(2)
                      << "{\"type\":\"metrics\",\"capture_fps\":"
                      << (captured - last_capture) / seconds
                      << ",\"rtsp_fps\":" << (pushed - last_rtsp) / seconds
                      << ",\"inference_fps\":" << (inferred - last_inference) / seconds
                      << ",\"inference_avg_ms\":" << average_inference_ms
                      << ",\"inference_input_avg_ms\":" << average_input_ms
                      << ",\"outstanding_leases\":" << metrics.outstanding_leases.load()
                      << ",\"outstanding_leases_peak\":"
                      << metrics.outstanding_leases_peak.load()
                      << ",\"encoder_access_units\":"
                      << metrics.encoder_access_units.load()
                      << ",\"encoder_output_bytes\":"
                      << metrics.encoder_output_bytes.load()
                      << ",\"encoder_dropped\":" << metrics.encoder_dropped.load()
                      << ",\"encoder_errors\":" << metrics.encoder_errors.load()
                      << ",\"encoder_recoveries\":"
                      << metrics.encoder_recoveries.load()
                      << ",\"encoder_recovery_failures\":"
                      << metrics.encoder_recovery_failures.load()
                      << ",\"rtsp_dropped\":" << metrics.rtsp_dropped.load()
                      << ",\"inference_dropped\":" << metrics.inference_dropped.load()
                      << ",\"inference_zero_copy_ingress\":"
                      << metrics.inference_zero_copy_ingress.load()
                      << ",\"inference_copy_fallback\":"
                      << metrics.inference_copy_fallback.load()
                      << ",\"inference_preprocess_errors\":"
                      << metrics.inference_preprocess_errors.load()
                      << ",\"inference_i420_rejections\":{\"invalid_dimensions\":"
                      << metrics.inference_i420_invalid_dimensions.load()
                      << ",\"unexpected_plane_count\":"
                      << metrics.inference_i420_unexpected_plane_count.load()
                      << ",\"unexpected_stride\":"
                      << metrics.inference_i420_unexpected_stride.load()
                      << ",\"invalid_fd\":" << metrics.inference_i420_invalid_fd.load()
                      << ",\"separate_fds\":" << metrics.inference_i420_separate_fds.load()
                      << ",\"missing_mapping\":"
                      << metrics.inference_i420_missing_mapping.load()
                      << ",\"inconsistent_mapping\":"
                      << metrics.inference_i420_inconsistent_mapping.load()
                      << ",\"non_compact_offsets\":"
                      << metrics.inference_i420_non_compact_offsets.load()
                      << ",\"plane_too_short\":"
                      << metrics.inference_i420_plane_too_short.load()
                      << ",\"payload_too_short\":"
                      << metrics.inference_i420_payload_too_short.load()
                      << ",\"mapping_too_short\":"
                      << metrics.inference_i420_mapping_too_short.load() << '}'
                      << ",\"capture_errors\":" << metrics.capture_errors.load()
                      << ",\"rtsp_errors\":" << metrics.rtsp_errors.load()
                      << ",\"rtsp_recoveries\":" << metrics.rtsp_recoveries.load()
                      << ",\"rtsp_recovery_failures\":"
                      << metrics.rtsp_recovery_failures.load()
                      << ",\"rtsp_sessions_current\":"
                      << metrics.rtsp_sessions_current.load()
                      << ",\"rtsp_sessions_peak\":" << metrics.rtsp_sessions_peak.load()
                      << ",\"rtsp_sessions_cleaned\":"
                      << metrics.rtsp_sessions_cleaned.load()
                      << ",\"encoded_ring_units\":" << ring.units
                      << ",\"encoded_ring_bytes\":" << ring.bytes
                      << ",\"encoded_ring_peak_bytes\":" << ring.peak_bytes
                      << ",\"encoded_ring_resets\":" << ring.resets
                      << ",\"events_triggered\":" << metrics.events_triggered.load()
                      << ",\"events_suppressed\":" << metrics.events_suppressed.load()
                      << ",\"events_completed\":" << metrics.events_completed.load()
                      << ",\"events_partial_preroll\":"
                      << metrics.events_partial_preroll.load()
                      << ",\"events_partial_postroll\":"
                      << metrics.events_partial_postroll.load()
                      << ",\"events_failed\":" << metrics.events_failed.load()
                      << ",\"event_video_bytes\":" << metrics.event_video_bytes.load()
                      << ",\"event_snapshot_bytes\":"
                      << metrics.event_snapshot_bytes.load()
                      << ",\"event_mux_errors\":" << metrics.event_mux_errors.load()
                      << ",\"event_snapshot_errors\":"
                      << metrics.event_snapshot_errors.load()
                      << ",\"event_disk_space_rejections\":"
                      << metrics.event_disk_space_rejections.load() << "}\n";
            last_capture = captured;
            last_rtsp = pushed;
            last_inference = inferred;
            last_report = now;
        }

        std::cout << "[app] graceful shutdown requested\n";
        camera.stop();          // Stop producing before releasing downstream buffers.
        inference.stop();
        encoder.stop();
        event_recorder.stop();
        rtsp.stop();
        std::cout << "[app] stopped captured=" << metrics.captured.load()
                  << " rtsp=" << metrics.rtsp_pushed.load()
                  << " inferred=" << metrics.inference_processed.load()
                  << " encoded=" << metrics.encoder_access_units.load()
                  << " outstanding=" << metrics.outstanding_leases.load()
                  << " outstanding_peak=" << metrics.outstanding_leases_peak.load()
                  << " inference_zero_copy=" << metrics.inference_zero_copy_ingress.load()
                  << " inference_copy_fallback=" << metrics.inference_copy_fallback.load()
                  << " inference_preprocess_errors="
                  << metrics.inference_preprocess_errors.load()
                  << " capture_errors=" << metrics.capture_errors.load()
                  << " encoder_errors=" << metrics.encoder_errors.load()
                  << " encoder_recoveries=" << metrics.encoder_recoveries.load()
                  << " encoder_recovery_failures="
                  << metrics.encoder_recovery_failures.load()
                  << " rtsp_errors=" << metrics.rtsp_errors.load()
                  << " rtsp_recoveries=" << metrics.rtsp_recoveries.load()
                  << " rtsp_recovery_failures="
                  << metrics.rtsp_recovery_failures.load()
                  << " rtsp_sessions_current=" << metrics.rtsp_sessions_current.load()
                  << " rtsp_sessions_peak=" << metrics.rtsp_sessions_peak.load()
                  << " rtsp_sessions_cleaned=" << metrics.rtsp_sessions_cleaned.load()
                  << " events_triggered=" << metrics.events_triggered.load()
                  << " events_completed=" << metrics.events_completed.load()
                  << " events_failed=" << metrics.events_failed.load()
                  << '\n';
        if (fatal_encoder_failure) {
            return 5;
        }
        return metrics.outstanding_leases.load() == 0 ? 0 : 4;
    } catch (const std::exception &error) {
        std::cerr << "[fatal] " << error.what() << '\n';
        usage(argv[0]);
        return 1;
    }
}
