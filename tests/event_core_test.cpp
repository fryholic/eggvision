#include "eggvision/encoded_ring_buffer.hpp"

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

}  // namespace

int main() {
    testConstructorValidation();
    testSelectionStartsAtPreviousIndependentFrame();
    testWarmupProducesPartialSelection();
    testGenerationAndTimestampReset();
    testTimeRetentionKeepsBoundaryKeyframe();
    testByteLimitAndInvalidUnits();

    if (failures == 0) {
        std::cout << "all event core tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
