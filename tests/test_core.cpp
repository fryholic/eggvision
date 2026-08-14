#include "eggvision/frame.hpp"
#include "eggvision/inference.hpp"
#include "eggvision/latest_frame_queue.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool near(float left, float right, float epsilon = 0.01F) {
    return std::fabs(left - right) <= epsilon;
}

}  // namespace

int main() {
    const auto transform = eggvision::calculateLetterbox(640, 480, 320, 320);
    expect(near(transform.scale, 0.5F), "letterbox scale");
    expect(transform.pad_x == 0 && transform.pad_y == 40, "letterbox padding");
    const auto restored = eggvision::restoreLetterboxBox({50, 65, 100, 50}, transform, 640, 480);
    expect(near(restored.x, 100) && near(restored.y, 50), "restored origin");
    expect(near(restored.width, 200) && near(restored.height, 100), "restored size");

    std::vector<eggvision::Detection> detections{
        {0, 0.90F, {10, 10, 100, 100}},
        {0, 0.80F, {15, 15, 100, 100}},
        {0, 0.70F, {250, 200, 30, 50}},
    };
    const auto nms = eggvision::nonMaximumSuppression(std::move(detections), 0.45F);
    expect(nms.size() == 2, "NMS suppresses overlapping lower confidence box");
    expect(near(nms.front().confidence, 0.90F), "NMS keeps highest confidence first");

    cv::Mat color(1, 1, CV_8UC3, cv::Scalar(10, 20, 30));
    float chw[3]{};
    eggvision::bgrToNormalizedRgbChw(color, chw);
    expect(near(chw[0], 30.0F / 255.0F), "BGR red channel maps to RGB CHW plane 0");
    expect(near(chw[1], 20.0F / 255.0F), "BGR green channel maps to RGB CHW plane 1");
    expect(near(chw[2], 10.0F / 255.0F), "BGR blue channel maps to RGB CHW plane 2");

    eggvision::LatestFrameQueue<int> queue;
    expect(!queue.push(1), "first latest-frame insert is not a replacement");
    expect(queue.push(2), "second latest-frame insert replaces old frame");
    int latest = 0;
    expect(queue.waitPop(latest) && latest == 2, "latest-frame queue returns newest item");
    queue.close();
    expect(!queue.push(3), "closed latest-frame queue rejects input");
    queue.reopen();
    expect(!queue.push(4), "reopened latest-frame queue accepts input");
    expect(queue.waitPop(latest) && latest == 4,
           "reopened latest-frame queue starts a fresh epoch");
    queue.close();

    int releases = 0;
    {
        eggvision::StreamView view;
        auto lease = std::make_shared<eggvision::FrameLease>(
            reinterpret_cast<libcamera::Request *>(static_cast<std::uintptr_t>(1)),
            view,
            view,
            1,
            2,
            [&releases](libcamera::Request *) { ++releases; });
        auto second_consumer = lease;
        lease.reset();
        expect(releases == 0, "FrameLease remains alive while one consumer holds it");
        second_consumer.reset();
    }
    expect(releases == 1, "FrameLease releases request exactly once");

    if (failures == 0) {
        std::cout << "all core tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
