#include "eggvision/encoded_ring_buffer.hpp"
#include "eggvision/h264_bitstream.hpp"
#include "eggvision/snapshot.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
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

eggvision::EncodedAccessUnitPtr unit(std::uint64_t timestamp,
                                    std::uint64_t generation,
                                    bool keyframe,
                                    std::size_t bytes = 100) {
    auto value = std::make_shared<eggvision::EncodedAccessUnit>();
    value->sensor_timestamp_ns = timestamp;
    value->pts_ns = timestamp;
    value->dts_ns = timestamp;
    value->duration_ns = 100;
    value->generation = generation;
    value->keyframe = keyframe;
    value->has_sps = keyframe;
    value->has_pps = keyframe;
    value->payload = std::make_shared<const eggvision::EncodedAccessUnit::Payload>(bytes, 0x5a);
    return value;
}

void testConstructorValidation() {
    bool retention_failed = false;
    try {
        eggvision::EncodedRingBuffer invalid(0, 1);
    } catch (const std::invalid_argument &) {
        retention_failed = true;
    }
    expect(retention_failed, "zero retention is rejected");

    bool bytes_failed = false;
    try {
        eggvision::EncodedRingBuffer invalid(1, 0);
    } catch (const std::invalid_argument &) {
        bytes_failed = true;
    }
    expect(bytes_failed, "zero byte limit is rejected");
}

void testSelectionStartsAtPreviousIndependentFrame() {
    eggvision::EncodedRingBuffer ring(4'000, 10'000);
    for (std::uint64_t timestamp = 1'000; timestamp <= 4'000; timestamp += 100) {
        ring.push(unit(timestamp, 1, timestamp == 1'000 || timestamp == 2'000 ||
                                         timestamp == 3'000 || timestamp == 4'000));
    }
    const auto selection = ring.selectPreRoll(3'750, 1'500);
    expect(selection.has_independent_start, "selection has an independent start");
    expect(selection.pre_roll_complete, "selection covers the requested pre-roll");
    expect(selection.requested_start_sensor_timestamp_ns == 2'250,
           "selection calculates requested start");
    expect(selection.actual_start_sensor_timestamp_ns == 2'000,
           "selection starts at previous IDR");
    expect(!selection.units.empty() && selection.units.front()->independentlyDecodable(),
           "selection begins with SPS/PPS IDR");
}

void testWarmupProducesPartialSelection() {
    eggvision::EncodedRingBuffer ring(4'000, 10'000);
    ring.push(unit(3'000, 1, true));
    ring.push(unit(3'100, 1, false));
    const auto selection = ring.selectPreRoll(3'200, 1'500);
    expect(selection.has_independent_start, "warm-up selection finds first IDR");
    expect(!selection.pre_roll_complete, "warm-up selection reports partial pre-roll");
    expect(selection.actual_start_sensor_timestamp_ns == 3'000,
           "warm-up selection keeps available IDR");
}

void testGenerationAndTimestampReset() {
    eggvision::EncodedRingBuffer ring(4'000, 10'000);
    expect(ring.push(unit(1'000, 1, true)) == eggvision::EncodedRingPushResult::Accepted,
           "first generation is accepted");
    expect(ring.push(unit(1'100, 2, true)) ==
               eggvision::EncodedRingPushResult::ResetForGeneration,
           "new generation resets ring");
    auto selection = ring.selectPreRoll(1'100, 100);
    expect(selection.generation == 2 && selection.units.size() == 1,
           "selection contains only new generation");

    expect(ring.push(unit(1'050, 2, true)) ==
               eggvision::EncodedRingPushResult::ResetForTimestampRegression,
           "timestamp regression resets ring");
    selection = ring.selectPreRoll(1'050, 100);
    expect(selection.units.size() == 1 &&
               selection.units.front()->sensor_timestamp_ns == 1'050,
           "selection contains only post-regression unit");
    expect(ring.stats().resets == 2, "ring counts both automatic resets");
}

void testTimeRetentionKeepsBoundaryKeyframe() {
    eggvision::EncodedRingBuffer ring(1'000, 10'000);
    ring.push(unit(1'000, 1, true));
    ring.push(unit(1'500, 1, false));
    ring.push(unit(2'000, 1, true));
    ring.push(unit(2'500, 1, false));
    ring.push(unit(3'000, 1, true));

    const auto selection = ring.selectPreRoll(3'000, 900);
    expect(selection.has_independent_start, "retained ring remains independently decodable");
    expect(selection.actual_start_sensor_timestamp_ns == 2'000,
           "time pruning retains keyframe before cutoff");
}

void testByteLimitAndInvalidUnits() {
    eggvision::EncodedRingBuffer ring(10'000, 350);
    ring.push(unit(1'000, 1, true, 100));
    ring.push(unit(1'100, 1, false, 100));
    ring.push(unit(2'000, 1, true, 100));
    ring.push(unit(2'100, 1, false, 100));

    const auto stats = ring.stats();
    expect(stats.bytes <= 350, "byte pruning enforces hard cap");
    const auto selection = ring.selectPreRoll(2'100, 1'000);
    expect(selection.has_independent_start &&
               selection.actual_start_sensor_timestamp_ns == 2'000,
           "byte pruning removes non-decodable prefix");

    expect(ring.push(nullptr) == eggvision::EncodedRingPushResult::RejectedInvalid,
           "null unit is rejected");
    expect(ring.push(unit(3'000, 1, true, 351)) ==
               eggvision::EncodedRingPushResult::RejectedOversize,
           "single oversize unit is rejected");
}

void testH264AnnexBInspection() {
    const std::vector<std::uint8_t> stream{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x01,
        0x00, 0x00, 0x01, 0x68, 0x02,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x03,
    };
    const auto summary = eggvision::inspectH264ByteStream(stream.data(), stream.size());
    expect(summary.has_sps, "Annex B parser finds SPS");
    expect(summary.has_pps, "Annex B parser finds PPS");
    expect(summary.has_idr, "Annex B parser finds IDR");

    const std::vector<std::uint8_t> delta{0x00, 0x00, 0x01, 0x41, 0x55};
    const auto delta_summary = eggvision::inspectH264ByteStream(delta.data(), delta.size());
    expect(!delta_summary.has_sps && !delta_summary.has_pps && !delta_summary.has_idr,
           "Annex B parser does not promote a delta slice");
}

void testMappedI420StrideCopy() {
    eggvision::StreamView view;
    view.width = 4;
    view.height = 4;
    view.stride = 8;
    std::vector<std::uint8_t> y{
        1, 2, 3, 4, 90, 90, 90, 90,
        5, 6, 7, 8, 90, 90, 90, 90,
        9, 10, 11, 12, 90, 90, 90, 90,
        13, 14, 15, 16, 90, 90, 90, 90,
    };
    std::vector<std::uint8_t> u{17, 18, 90, 90, 19, 20, 90, 90};
    std::vector<std::uint8_t> v{21, 22, 90, 90, 23, 24, 90, 90};
    view.planes = {
        {-1, 0, static_cast<std::uint32_t>(y.size()),
         static_cast<std::uint32_t>(y.size()), y.data()},
        {-1, 0, static_cast<std::uint32_t>(u.size()),
         static_cast<std::uint32_t>(u.size()), u.data()},
        {-1, 0, static_cast<std::uint32_t>(v.size()),
         static_cast<std::uint32_t>(v.size()), v.data()},
    };
    std::vector<std::uint8_t> packed;
    std::string error;
    expect(eggvision::copyMappedI420(view, packed, error), "mapped I420 copy succeeds");
    const std::vector<std::uint8_t> expected{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24,
    };
    expect(packed == expected, "mapped I420 copy removes luma and chroma stride padding");
}

}  // namespace

int main() {
    testConstructorValidation();
    testSelectionStartsAtPreviousIndependentFrame();
    testWarmupProducesPartialSelection();
    testGenerationAndTimestampReset();
    testTimeRetentionKeepsBoundaryKeyframe();
    testByteLimitAndInvalidUnits();
    testH264AnnexBInspection();
    testMappedI420StrideCopy();

    if (failures == 0) {
        std::cout << "all event core tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
