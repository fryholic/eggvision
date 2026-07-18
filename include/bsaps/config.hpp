#pragma once

#include <cstdint>
#include <string>

namespace bsaps {

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
    unsigned bitrate = 4'000'000;
    unsigned gop = 30;

    bool inference_enabled = true;
    std::string model_path = "models/yolov5n.xml";
    float confidence_threshold = 0.30F;
    float nms_threshold = 0.45F;
    unsigned inference_width = 320;
    unsigned inference_height = 320;

    unsigned duration_seconds = 0;
    unsigned metrics_interval_seconds = 5;
};

}  // namespace bsaps

