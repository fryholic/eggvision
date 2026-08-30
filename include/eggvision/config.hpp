#pragma once

#include <cstdint>
#include <string>

namespace eggvision {

struct AppConfig {
    unsigned main_width = 1920;
    unsigned main_height = 1080;
    unsigned lores_width = 640;
    unsigned lores_height = 480;
    unsigned fps = 30;
    unsigned buffer_count = 8;

    std::string rtsp_address = "0.0.0.0";
    std::string rtsp_port = "8554";
    std::string rtsp_mount = "/stream";
    unsigned rtsp_max_sessions = 32;
    unsigned bitrate = 4'000'000;
    unsigned gop = 12;

    bool inference_enabled = true;
    std::string inference_backend = "mnn";
    std::string model_path = "models/yolov5n.mnn";
    float confidence_threshold = 0.30F;
    float nms_threshold = 0.45F;
    unsigned inference_width = 320;
    unsigned inference_height = 320;
    unsigned inference_threads = 3;

    bool event_recording_enabled = true;
    std::string events_dir = "/var/lib/eggvision/events";
    double event_pre_seconds = 1.5;
    double event_post_seconds = 1.5;
    double event_cooldown_seconds = 10.0;
    double event_ring_seconds = 4.0;
    std::uint64_t event_ring_max_bytes = 8ULL * 1024ULL * 1024ULL;
    std::uint64_t event_min_free_bytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;
    int event_jpeg_quality = 90;
    std::string event_container = "mp4";

    unsigned duration_seconds = 0;
    unsigned metrics_interval_seconds = 5;
};

}  // namespace eggvision
