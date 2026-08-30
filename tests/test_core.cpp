#include "eggvision/dma_buf_sync.hpp"
#include "eggvision/frame.hpp"
#include "eggvision/i420.hpp"
#include "eggvision/inference.hpp"
#include "eggvision/latest_frame_queue.hpp"
#include "eggvision/logging.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <linux/dma-buf.h>
#include <opencv2/imgproc.hpp>

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

    const std::uint32_t required_payloads[] = {16, 4, 4};
    for (std::size_t plane_index = 0; plane_index < compatible.planes.size(); ++plane_index) {
        auto short_payload = compatible;
        short_payload.planes[plane_index].bytes_used = required_payloads[plane_index] - 1;
        expect(eggvision::inspectCompactI420(short_payload).status ==
                   eggvision::CompactI420Status::PayloadTooShort,
               "short I420 payload rejects zero-copy ingress for plane " +
                   std::to_string(plane_index));

        std::vector<std::uint8_t> rejected;
        std::string payload_error;
        expect(!eggvision::copyMappedI420(short_payload, rejected, payload_error),
               "copy fallback rejects a short I420 payload for plane " +
                   std::to_string(plane_index));
    }

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

    cv::Mat direct_i420(6, 4, CV_8UC1, storage.data());
    cv::Mat copied_i420(6, 4, CV_8UC1, packed.data());
    cv::Mat direct_bgr;
    cv::Mat copied_bgr;
    cv::cvtColor(direct_i420, direct_bgr, cv::COLOR_YUV2BGR_I420);
    cv::cvtColor(copied_i420, copied_bgr, cv::COLOR_YUV2BGR_I420);
    expect(cv::norm(direct_bgr, copied_bgr, cv::NORM_INF) == 0.0,
           "zero-copy and fallback I420 conversions are bit-identical");

    expect(!eggvision::copyMappedI420(short_plane, packed, error),
           "copy fallback rejects a short plane without reading past it");

    eggvision::StreamView single_plane = compatible;
    single_plane.planes = {
        {17,
         0,
         static_cast<std::uint32_t>(storage.size()),
         static_cast<std::uint32_t>(storage.size()),
         storage.data(),
         storage.data(),
         storage.size()},
    };
    expect(eggvision::copyMappedI420(single_plane, packed, error),
           "single-plane copy accepts an exact payload boundary");
    single_plane.planes[0].bytes_used = static_cast<std::uint32_t>(storage.size() - 1);
    expect(!eggvision::copyMappedI420(single_plane, packed, error),
           "single-plane copy rejects a short payload");
}

struct SyncCall {
    int fd = -1;
    std::uint64_t flags = 0;
};

struct SyncRecorder {
    std::vector<SyncCall> calls;
    int fail_fd = -1;
    std::uint64_t fail_flags = 0;
    int fail_error = EIO;
    unsigned failures_remaining = 0;
};

int recordSync(int fd, std::uint64_t flags, void *context) {
    auto &recorder = *static_cast<SyncRecorder *>(context);
    recorder.calls.push_back({fd, flags});
    if (recorder.failures_remaining > 0 && recorder.fail_fd == fd &&
        recorder.fail_flags == flags) {
        --recorder.failures_remaining;
        return recorder.fail_error;
    }
    return 0;
}

void testDmaBufReadSync() {
    std::vector<std::uint8_t> storage;
    const eggvision::StreamView shared_fd = compactI420Fixture(storage);
    const std::uint64_t start = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    const std::uint64_t end = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;

    SyncRecorder shared_recorder;
    std::string error;
    {
        eggvision::DmaBufReadSync sync(shared_fd, error, recordSync, &shared_recorder);
        expect(static_cast<bool>(sync), "DMA-BUF read sync starts for a shared FD");
        expect(shared_recorder.calls.size() == 1 && shared_recorder.calls[0].fd == 17 &&
                   shared_recorder.calls[0].flags == start,
               "shared plane FD is synchronized once");
        expect(sync.finish(error), "DMA-BUF read sync ends successfully");
    }
    expect(shared_recorder.calls.size() == 2 && shared_recorder.calls[1].fd == 17 &&
               shared_recorder.calls[1].flags == end,
           "shared plane FD receives one matching END");

    auto separate_fds = shared_fd;
    separate_fds.planes[1].fd = 18;
    separate_fds.planes[2].fd = 19;
    SyncRecorder separate_recorder;
    {
        eggvision::DmaBufReadSync sync(separate_fds, error, recordSync, &separate_recorder);
        expect(static_cast<bool>(sync), "DMA-BUF read sync starts for separate FDs");
        expect(sync.finish(error), "separate DMA-BUF read sync ends successfully");
    }
    const int expected_fds[] = {17, 18, 19, 19, 18, 17};
    const std::uint64_t expected_flags[] = {start, start, start, end, end, end};
    expect(separate_recorder.calls.size() == 6,
           "each separate FD receives one START and one END");
    for (std::size_t i = 0; i < separate_recorder.calls.size() && i < 6; ++i) {
        expect(separate_recorder.calls[i].fd == expected_fds[i] &&
                   separate_recorder.calls[i].flags == expected_flags[i],
               "separate DMA-BUF sync order " + std::to_string(i));
    }

    SyncRecorder partial_failure;
    partial_failure.fail_fd = 18;
    partial_failure.fail_flags = start;
    partial_failure.failures_remaining = 1;
    {
        eggvision::DmaBufReadSync sync(separate_fds, error, recordSync, &partial_failure);
        expect(!static_cast<bool>(sync), "middle DMA-BUF START failure is reported");
    }
    expect(partial_failure.calls.size() == 3 && partial_failure.calls[0].fd == 17 &&
               partial_failure.calls[0].flags == start && partial_failure.calls[1].fd == 18 &&
               partial_failure.calls[1].flags == start && partial_failure.calls[2].fd == 17 &&
               partial_failure.calls[2].flags == end,
           "middle START failure ends every previously-started FD");

    SyncRecorder exception_cleanup;
    try {
        eggvision::DmaBufReadSync sync(shared_fd, error, recordSync, &exception_cleanup);
        expect(static_cast<bool>(sync), "exception cleanup fixture starts DMA-BUF sync");
        throw std::runtime_error("synthetic conversion failure");
    } catch (const std::runtime_error &) {
    }
    expect(exception_cleanup.calls.size() == 2 && exception_cleanup.calls[1].flags == end,
           "DMA-BUF read sync destructor ends access during exception unwinding");

    SyncRecorder retry;
    retry.fail_fd = 17;
    retry.fail_flags = start;
    retry.fail_error = EAGAIN;
    retry.failures_remaining = 1;
    {
        eggvision::DmaBufReadSync sync(shared_fd, error, recordSync, &retry);
        expect(static_cast<bool>(sync), "retryable DMA-BUF START failure is retried");
        expect(sync.finish(error), "retried DMA-BUF sync ends successfully");
    }
    expect(retry.calls.size() == 3 && retry.calls[0].flags == start &&
               retry.calls[1].flags == start && retry.calls[2].flags == end,
           "EAGAIN repeats START before the matching END");
}

}  // namespace

int main() {
    std::ostringstream concurrent_log;
    std::vector<std::thread> log_writers;
    constexpr int kLogThreads = 8;
    constexpr int kLogLinesPerThread = 100;
    for (int thread = 0; thread < kLogThreads; ++thread) {
        log_writers.emplace_back([thread, &concurrent_log] {
            for (int line = 0; line < kLogLinesPerThread; ++line) {
                eggvision::synchronizedLog(concurrent_log)
                    << "{\"thread\":" << thread << ",\"line\":" << line << "}\n";
            }
        });
    }
    for (auto &writer : log_writers) {
        writer.join();
    }
    std::set<std::string> observed_log_lines;
    std::istringstream concurrent_log_input(concurrent_log.str());
    std::string concurrent_line;
    bool log_lines_valid = true;
    while (std::getline(concurrent_log_input, concurrent_line)) {
        const std::size_t separator = concurrent_line.find(",\"line\":");
        log_lines_valid = log_lines_valid && concurrent_line.rfind("{\"thread\":", 0) == 0 &&
                          separator != std::string::npos && concurrent_line.back() == '}';
        observed_log_lines.insert(concurrent_line);
    }
    expect(log_lines_valid, "synchronized logger preserves complete JSON lines");
    expect(observed_log_lines.size() == kLogThreads * kLogLinesPerThread,
           "synchronized logger preserves every concurrent line exactly once");

    namespace fs = std::filesystem;
    const fs::path fingerprint_dir =
        fs::temp_directory_path() / "eggvision-model-fingerprint-test";
    std::error_code fingerprint_error;
    fs::remove_all(fingerprint_dir, fingerprint_error);
    fs::create_directory(fingerprint_dir);
    const fs::path mnn_model = fingerprint_dir / "model.mnn";
    const fs::path openvino_xml = fingerprint_dir / "model.xml";
    const fs::path openvino_bin = fingerprint_dir / "model.bin";
    {
        std::ofstream(mnn_model, std::ios::binary) << "mnn-model";
        std::ofstream(openvino_xml, std::ios::binary) << "openvino-graph";
        std::ofstream(openvino_bin, std::ios::binary) << "weights-a";
    }
    const std::string mnn_fingerprint =
        eggvision::inferenceModelFingerprint("mnn", mnn_model.string());
    const std::string openvino_fingerprint_a =
        eggvision::inferenceModelFingerprint("openvino", openvino_xml.string());
    std::ofstream(openvino_bin, std::ios::binary | std::ios::trunc) << "weights-b";
    const std::string openvino_fingerprint_b =
        eggvision::inferenceModelFingerprint("openvino", openvino_xml.string());
    expect(mnn_fingerprint.size() == 64,
           "MNN fingerprint is the single model SHA-256");
    expect(openvino_fingerprint_a.find("xml:") == 0 &&
               openvino_fingerprint_a.find(",bin:") != std::string::npos,
           "OpenVINO fingerprint identifies graph and weights");
    expect(openvino_fingerprint_a != openvino_fingerprint_b,
           "OpenVINO fingerprint changes when only BIN weights change");
    fs::remove_all(fingerprint_dir, fingerprint_error);

    testDmaBufReadSync();
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
