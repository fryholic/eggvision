#include "eggvision/camera_capture.hpp"
#include "eggvision/config.hpp"
#include "eggvision/encoded_ring_buffer.hpp"
#include "eggvision/event_recorder.hpp"
#include "eggvision/h264_encoder.hpp"
#include "eggvision/metrics.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("usage: eggvision_event_recording_test EVENTS_DIR");
        }
        eggvision::AppConfig config;
        config.inference_enabled = false;
        config.events_dir = argv[1];
        config.event_min_free_bytes = 1024 * 1024;
        config.event_cooldown_seconds = 0.0;

        eggvision::Metrics metrics;
        eggvision::EncodedRingBuffer history(4ULL * 1'000'000'000ULL,
                                             8ULL * 1024ULL * 1024ULL);
        eggvision::EventRecorder recorder(config, history, metrics);
        eggvision::H264Encoder encoder(config, metrics);
        eggvision::CameraCapture camera(config, metrics);
        if (!recorder.initialize() || !encoder.initialize() || !camera.initialize() ||
            !recorder.start() || !encoder.start()) {
            throw std::runtime_error("event harness initialization failed");
        }

        encoder.setConsumer([&](eggvision::EncodedAccessUnitPtr unit) {
            history.push(unit);
            recorder.observeEncoded(unit);
        });
        std::atomic<std::uint64_t> first_timestamp{0};
        std::atomic<bool> triggered{false};
        camera.setMainConsumer([&](std::shared_ptr<eggvision::FrameLease> frame) {
            std::uint64_t first = first_timestamp.load();
            if (first == 0) {
                first_timestamp.compare_exchange_strong(first, frame->sensorTimestampNs());
                first = first_timestamp.load();
            }
            if (!triggered.load() &&
                frame->sensorTimestampNs() >= first + 2ULL * 1'000'000'000ULL) {
                eggvision::Detection detection;
                detection.class_id = 0;
                detection.confidence = 0.99F;
                detection.box = {100.0F, 80.0F, 200.0F, 300.0F};
                if (recorder.trigger(frame, {detection})) {
                    triggered.store(true);
                }
            }
            encoder.submit(std::move(frame));
        });
        if (!camera.start() ||
            !encoder.waitForIndependentFrame(std::chrono::seconds(5))) {
            throw std::runtime_error("camera/encoder did not produce an independent frame");
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (std::chrono::steady_clock::now() < deadline &&
               metrics.events_completed.load() == 0 && metrics.events_failed.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        camera.stop();
        encoder.stop();
        recorder.stop();

        if (!triggered.load()) {
            throw std::runtime_error("deterministic trigger was not accepted");
        }
        if (metrics.events_completed.load() != 1 || metrics.events_failed.load() != 0) {
            throw std::runtime_error("event did not finalize successfully");
        }
        if (metrics.event_video_bytes.load() == 0 ||
            metrics.event_snapshot_bytes.load() == 0) {
            throw std::runtime_error("event artifacts are empty");
        }
        if (metrics.outstanding_leases.load() != 0) {
            throw std::runtime_error("event harness retained a camera lease");
        }
        std::cout << "[event-recording-test] passed root=" << config.events_dir
                  << " video_bytes=" << metrics.event_video_bytes.load()
                  << " snapshot_bytes=" << metrics.event_snapshot_bytes.load()
                  << " outstanding=0\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[event-recording-test] FAIL: " << error.what() << '\n';
        return 1;
    }
}
