#include "eggvision/frame.hpp"
#include "eggvision/i420.hpp"
#include "eggvision/inference.hpp"
#include "eggvision/latest_frame_queue.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
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

std::uint32_t floatBits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint32_t ulpDistance(float left, float right) {
    const std::uint32_t left_bits = floatBits(left);
    const std::uint32_t right_bits = floatBits(right);
    return left_bits > right_bits ? left_bits - right_bits : right_bits - left_bits;
}

void expectNormalizationMatchesDivision(const cv::Mat &bgr, const std::string &case_name) {
    const std::size_t plane_size = static_cast<std::size_t>(bgr.cols) * bgr.rows;
    std::vector<float> actual(plane_size * 3);
    eggvision::bgrToNormalizedRgbChw(bgr, actual.data());

    std::uint32_t max_ulp = 0;
    float max_absolute_error = 0.0F;
    for (int y = 0; y < bgr.rows; ++y) {
        const auto *row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * bgr.cols + x;
            const float expected[] = {
                static_cast<float>(row[x][2]) / 255.0F,
                static_cast<float>(row[x][1]) / 255.0F,
                static_cast<float>(row[x][0]) / 255.0F,
            };
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float observed = actual[channel * plane_size + index];
                max_ulp = std::max(max_ulp, ulpDistance(observed, expected[channel]));
                max_absolute_error =
                    std::max(max_absolute_error, std::fabs(observed - expected[channel]));
            }
        }
    }

    std::ostringstream result;
    result << case_name << " normalization differs from division by max_ulp=" << max_ulp
           << " max_absolute_error=" << max_absolute_error;
    expect(max_ulp <= 1 && max_absolute_error <= std::numeric_limits<float>::epsilon() / 2.0F,
           result.str());
}

eggvision::StreamView compactI420Fixture(std::vector<std::uint8_t> &storage) {
    storage.resize(24);
    for (std::size_t i = 0; i < storage.size(); ++i) {
        storage[i] = static_cast<std::uint8_t>(i);
    }
    eggvision::StreamView view;
    view.width = 4;
    view.height = 4;
    view.stride = 4;
    view.frame_size = static_cast<unsigned>(storage.size());
    view.planes = {
        {17, 0, 16, 16, storage.data(), storage.data(), storage.size()},
        {17, 16, 4, 4, storage.data() + 16, storage.data(), storage.size()},
        {17, 20, 4, 4, storage.data() + 20, storage.data(), storage.size()},
    };
    return view;
}

void testCompactI420Inspection() {
    std::vector<std::uint8_t> storage;
    const eggvision::StreamView compatible = compactI420Fixture(storage);
    const eggvision::CompactI420View result = eggvision::inspectCompactI420(compatible);
    expect(result.status == eggvision::CompactI420Status::Compatible,
           "compact I420 layout is accepted");
    expect(result.data == storage.data() && result.size == storage.size(),
           "compact I420 view spans the shared mapping");
    expect(std::string(eggvision::compactI420StatusName(result.status)) == "compatible",
           "compact I420 status has a stable metric name");

    auto separate_fds = compatible;
    separate_fds.planes[2].fd = 18;
    expect(eggvision::inspectCompactI420(separate_fds).status ==
               eggvision::CompactI420Status::DifferentFileDescriptors,
           "separate I420 file descriptors reject zero-copy ingress");

    auto non_compact = compatible;
    ++non_compact.planes[1].offset;
    ++non_compact.planes[1].data;
    expect(eggvision::inspectCompactI420(non_compact).status ==
               eggvision::CompactI420Status::NonCompactOffsets,
           "gapped I420 offsets reject zero-copy ingress");

    auto padded = compatible;
    padded.stride = 6;
    expect(eggvision::inspectCompactI420(padded).status ==
               eggvision::CompactI420Status::UnexpectedStride,
           "padded I420 stride rejects zero-copy ingress");

    auto short_mapping = compatible;
    for (auto &plane : short_mapping.planes) {
        plane.mapped_length = storage.size() - 1;
    }
    expect(eggvision::inspectCompactI420(short_mapping).status ==
               eggvision::CompactI420Status::MappingTooShort,
           "short DMA-BUF mapping rejects zero-copy ingress");

    auto short_plane = compatible;
    short_plane.planes[2].length = 3;
    expect(eggvision::inspectCompactI420(short_plane).status ==
               eggvision::CompactI420Status::PlaneTooShort,
           "short I420 plane rejects zero-copy ingress");

    auto missing_mapping = compatible;
    missing_mapping.planes[1].mapping_base = nullptr;
    expect(eggvision::inspectCompactI420(missing_mapping).status ==
               eggvision::CompactI420Status::MissingMapping,
           "unmapped I420 plane rejects zero-copy ingress");

    std::vector<std::uint8_t> packed;
    std::string error;
    expect(eggvision::copyMappedI420(compatible, packed, error),
           "compatible I420 view can use the copy fallback");
    expect(packed == storage, "copy fallback preserves compact I420 bytes");
    expect(!eggvision::copyMappedI420(short_plane, packed, error),
           "copy fallback rejects a short plane without reading past it");
}

}  // namespace

int main() {
    testCompactI420Inspection();

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

    cv::Mat all_values(1, 256, CV_8UC3);
    for (int value = 0; value <= 255; ++value) {
        all_values.at<cv::Vec3b>(0, value) = cv::Vec3b(
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(255 - value),
            static_cast<std::uint8_t>((value * 127) % 256));
    }
    expectNormalizationMatchesDivision(all_values, "all u8 values");

    cv::Mat random_storage(322, 324, CV_8UC3);
    std::mt19937 random(0x45564732U);
    for (int y = 0; y < random_storage.rows; ++y) {
        auto *row = random_storage.ptr<cv::Vec3b>(y);
        for (int x = 0; x < random_storage.cols; ++x) {
            row[x] = cv::Vec3b(static_cast<std::uint8_t>(random() & 0xffU),
                               static_cast<std::uint8_t>(random() & 0xffU),
                               static_cast<std::uint8_t>(random() & 0xffU));
        }
    }
    const cv::Mat random_non_contiguous = random_storage(cv::Rect(2, 1, 320, 320));
    expect(!random_non_contiguous.isContinuous(), "random normalization fixture has row stride");
    expectNormalizationMatchesDivision(random_non_contiguous, "random 320x320 ROI");

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
