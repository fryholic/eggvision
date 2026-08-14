#include "eggvision/camera_capture.hpp"
#include "eggvision/config.hpp"
#include "eggvision/encoded_ring_buffer.hpp"
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
        << "  --confidence VALUE    person confidence threshold (default 0.30)\n"
        << "  --nms VALUE           NMS IoU threshold (default 0.45)\n"
        << "  --inference-threads N OpenVINO CPU threads (default 2)\n"
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
        } else if (argument == "--confidence") {
            config.confidence_threshold = std::stof(value(i));
        } else if (argument == "--nms") {
            config.nms_threshold = std::stof(value(i));
        } else if (argument == "--inference-threads") {
            config.inference_threads = static_cast<unsigned>(std::stoul(value(i)));
        } else if (argument == "--duration") {
            config.duration_seconds = static_cast<unsigned>(std::stoul(value(i)));
        } else if (argument == "--no-inference") {
            config.inference_enabled = false;
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
        eggvision::EncodedRingBuffer encoded_history(4ULL * 1'000'000'000ULL,
                                                      8ULL * 1024ULL * 1024ULL);
        eggvision::InferenceWorker inference(config, metrics);
        eggvision::CameraCapture camera(config, metrics);

        if (!inference.initialize() || !encoder.initialize() || !camera.initialize()) {
            return 2;
        }
        encoder.setConsumer([&rtsp, &encoded_history](eggvision::EncodedAccessUnitPtr unit) {
            encoded_history.push(unit);
            rtsp.submit(std::move(unit));
        });
        camera.setMainConsumer([&encoder](std::shared_ptr<eggvision::FrameLease> frame) {
            encoder.submit(std::move(frame));
        });
        if (config.inference_enabled) {
            camera.setInferenceConsumer([&inference](std::shared_ptr<eggvision::FrameLease> frame) {
                inference.submit(std::move(frame));
            });
        }

        // Do not expose the RTSP listener until the camera is producing frames;
        // otherwise an eager client can make the first DESCRIBE fail with 503.
        if (!encoder.start() || !inference.start() || !camera.start() ||
            !encoder.waitForIndependentFrame(std::chrono::seconds(5)) || !rtsp.start()) {
            camera.stop();
            inference.stop();
            encoder.stop();
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
        while (!exit_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
            const double average_inference_ms = inferred == 0
                                                    ? 0.0
                                                    : metrics.inference_total_us.load() / 1000.0 / inferred;
            std::cout << std::fixed << std::setprecision(2)
                      << "{\"type\":\"metrics\",\"capture_fps\":"
                      << (captured - last_capture) / seconds
                      << ",\"rtsp_fps\":" << (pushed - last_rtsp) / seconds
                      << ",\"inference_fps\":" << (inferred - last_inference) / seconds
                      << ",\"inference_avg_ms\":" << average_inference_ms
                      << ",\"outstanding_leases\":" << metrics.outstanding_leases.load()
                      << ",\"encoder_access_units\":"
                      << metrics.encoder_access_units.load()
                      << ",\"encoder_output_bytes\":"
                      << metrics.encoder_output_bytes.load()
                      << ",\"encoder_dropped\":" << metrics.encoder_dropped.load()
                      << ",\"encoder_errors\":" << metrics.encoder_errors.load()
                      << ",\"rtsp_dropped\":" << metrics.rtsp_dropped.load()
                      << ",\"inference_dropped\":" << metrics.inference_dropped.load()
                      << ",\"capture_errors\":" << metrics.capture_errors.load()
                      << ",\"rtsp_errors\":" << metrics.rtsp_errors.load()
                      << ",\"rtsp_recoveries\":" << metrics.rtsp_recoveries.load()
                      << ",\"rtsp_recovery_failures\":"
                      << metrics.rtsp_recovery_failures.load()
                      << ",\"rtsp_sessions_current\":"
                      << metrics.rtsp_sessions_current.load()
                      << ",\"rtsp_sessions_peak\":" << metrics.rtsp_sessions_peak.load()
                      << ",\"rtsp_sessions_cleaned\":"
                      << metrics.rtsp_sessions_cleaned.load() << "}\n";
            last_capture = captured;
            last_rtsp = pushed;
            last_inference = inferred;
            last_report = now;
        }

        std::cout << "[app] graceful shutdown requested\n";
        camera.stop();          // Stop producing before releasing downstream buffers.
        inference.stop();
        encoder.stop();
        rtsp.stop();
        std::cout << "[app] stopped captured=" << metrics.captured.load()
                  << " rtsp=" << metrics.rtsp_pushed.load()
                  << " inferred=" << metrics.inference_processed.load()
                  << " encoded=" << metrics.encoder_access_units.load()
                  << " outstanding=" << metrics.outstanding_leases.load()
                  << " capture_errors=" << metrics.capture_errors.load()
                  << " encoder_errors=" << metrics.encoder_errors.load()
                  << " rtsp_errors=" << metrics.rtsp_errors.load()
                  << " rtsp_recoveries=" << metrics.rtsp_recoveries.load()
                  << " rtsp_recovery_failures="
                  << metrics.rtsp_recovery_failures.load()
                  << " rtsp_sessions_current=" << metrics.rtsp_sessions_current.load()
                  << " rtsp_sessions_peak=" << metrics.rtsp_sessions_peak.load()
                  << " rtsp_sessions_cleaned=" << metrics.rtsp_sessions_cleaned.load()
                  << '\n';
        return metrics.outstanding_leases.load() == 0 ? 0 : 4;
    } catch (const std::exception &error) {
        std::cerr << "[fatal] " << error.what() << '\n';
        usage(argv[0]);
        return 1;
    }
}
