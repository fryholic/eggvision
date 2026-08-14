#include "eggvision/camera_capture.hpp"
#include "eggvision/config.hpp"
#include "eggvision/metrics.hpp"
#include "eggvision/snapshot.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>

int main() {
    try {
        eggvision::AppConfig config;
        config.inference_enabled = false;
        eggvision::Metrics metrics;
        eggvision::CameraCapture camera(config, metrics);
        if (!camera.initialize()) {
            throw std::runtime_error("camera initialization failed");
        }

        std::mutex mutex;
        std::condition_variable cv;
        bool attempted = false;
        bool staged = false;
        std::string error;
        eggvision::MainSnapshot snapshot;
        camera.setMainConsumer([&](std::shared_ptr<eggvision::FrameLease> frame) {
            std::lock_guard<std::mutex> lock(mutex);
            if (attempted) {
                return;
            }
            attempted = true;
            staged = eggvision::stageMainSnapshot(*frame, snapshot, error);
            cv.notify_all();
        });
        if (!camera.start()) {
            throw std::runtime_error("camera start failed");
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return attempted; })) {
                camera.stop();
                throw std::runtime_error("snapshot timeout");
            }
        }
        camera.stop();
        if (!staged) {
            throw std::runtime_error("snapshot staging failed: " + error);
        }
        const std::size_t expected =
            static_cast<std::size_t>(config.main_width) * config.main_height * 3 / 2;
        if (snapshot.width != config.main_width || snapshot.height != config.main_height ||
            snapshot.i420.size() != expected) {
            throw std::runtime_error("snapshot layout mismatch");
        }
        const std::uint64_t checksum =
            std::accumulate(snapshot.i420.begin(), snapshot.i420.end(), std::uint64_t{0});
        if (checksum == 0) {
            throw std::runtime_error("snapshot pixels are empty");
        }
        if (metrics.outstanding_leases.load() != 0) {
            throw std::runtime_error("snapshot retained a camera lease");
        }
        std::cout << "[snapshot-test] passed sequence=" << snapshot.sequence
                  << " timestamp_ns=" << snapshot.sensor_timestamp_ns
                  << " bytes=" << snapshot.i420.size()
                  << " checksum=" << checksum << " outstanding=0\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[snapshot-test] FAIL: " << error.what() << '\n';
        return 1;
    }
}
