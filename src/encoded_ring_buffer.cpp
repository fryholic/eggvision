#include "eggvision/encoded_ring_buffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace eggvision {

EncodedRingBuffer::EncodedRingBuffer(std::uint64_t retention_ns, std::size_t max_bytes)
    : retention_ns_(retention_ns), max_bytes_(max_bytes) {
    if (retention_ns_ == 0) {
        throw std::invalid_argument("encoded ring retention must be positive");
    }
    if (max_bytes_ == 0) {
        throw std::invalid_argument("encoded ring byte limit must be positive");
    }
}

EncodedRingPushResult EncodedRingBuffer::push(EncodedAccessUnitPtr unit) {
    if (!unit || !unit->payload || unit->payload->empty() || unit->generation == 0 ||
        unit->sensor_timestamp_ns == 0) {
        return EncodedRingPushResult::RejectedInvalid;
    }
    if (unit->sizeBytes() > max_bytes_) {
        return EncodedRingPushResult::RejectedOversize;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    EncodedRingPushResult result = EncodedRingPushResult::Accepted;
    if (!units_.empty() && unit->generation != generation_) {
        resetLocked();
        result = EncodedRingPushResult::ResetForGeneration;
    } else if (!units_.empty() && unit->sensor_timestamp_ns < latest_sensor_timestamp_ns_) {
        resetLocked();
        result = EncodedRingPushResult::ResetForTimestampRegression;
    }

    generation_ = unit->generation;
    latest_sensor_timestamp_ns_ = unit->sensor_timestamp_ns;
    bytes_ += unit->sizeBytes();
    units_.push_back(std::move(unit));

    pruneByTimeLocked();
    pruneByBytesLocked();
    peak_bytes_ = std::max(peak_bytes_, bytes_);
    return result;
}

EncodedRingSelection EncodedRingBuffer::selectPreRoll(
    std::uint64_t trigger_sensor_timestamp_ns,
    std::uint64_t pre_roll_ns) const {
    EncodedRingSelection selection;
    selection.requested_start_sensor_timestamp_ns =
        trigger_sensor_timestamp_ns > pre_roll_ns
            ? trigger_sensor_timestamp_ns - pre_roll_ns
            : 0;

    std::lock_guard<std::mutex> lock(mutex_);
    if (units_.empty()) {
        return selection;
    }
    selection.generation = generation_;

    // Prefer the newest independently decodable IDR at or before the requested
    // boundary. If warm-up left no such unit, use the first decodable IDR that
    // still precedes the trigger and explicitly mark the pre-roll as partial.
    std::size_t start = units_.size();
    for (std::size_t index = 0; index < units_.size(); ++index) {
        const auto &unit = units_[index];
        if (unit->sensor_timestamp_ns > selection.requested_start_sensor_timestamp_ns) {
            break;
        }
        if (unit->independentlyDecodable()) {
            start = index;
        }
    }
    if (start == units_.size()) {
        for (std::size_t index = 0; index < units_.size(); ++index) {
            const auto &unit = units_[index];
            if (unit->sensor_timestamp_ns > trigger_sensor_timestamp_ns) {
                break;
            }
            if (unit->independentlyDecodable()) {
                start = index;
                break;
            }
        }
    }
    if (start == units_.size()) {
        return selection;
    }

    selection.actual_start_sensor_timestamp_ns = units_[start]->sensor_timestamp_ns;
    selection.pre_roll_complete =
        selection.actual_start_sensor_timestamp_ns <=
        selection.requested_start_sensor_timestamp_ns;
    selection.has_independent_start = true;
    selection.units.reserve(units_.size() - start);
    for (std::size_t index = start; index < units_.size(); ++index) {
        selection.units.push_back(units_[index]);
    }
    return selection;
}

void EncodedRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    resetLocked();
}

EncodedRingStats EncodedRingBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {units_.size(), bytes_, peak_bytes_, resets_};
}

void EncodedRingBuffer::resetLocked() {
    units_.clear();
    bytes_ = 0;
    generation_ = 0;
    latest_sensor_timestamp_ns_ = 0;
    ++resets_;
}

void EncodedRingBuffer::pruneByTimeLocked() {
    if (units_.size() < 2 || latest_sensor_timestamp_ns_ <= retention_ns_) {
        return;
    }
    const std::uint64_t cutoff = latest_sensor_timestamp_ns_ - retention_ns_;

    // Retain the newest independently-decodable IDR at or before the cutoff so
    // a selection near the retention boundary can still start cleanly.
    std::size_t keep = 0;
    bool found_keyframe = false;
    for (std::size_t index = 0; index < units_.size(); ++index) {
        if (units_[index]->sensor_timestamp_ns > cutoff) {
            break;
        }
        if (units_[index]->independentlyDecodable()) {
            keep = index;
            found_keyframe = true;
        }
    }
    if (!found_keyframe) {
        while (units_.size() > 1 && units_.front()->sensor_timestamp_ns < cutoff) {
            discardFrontLocked();
        }
        return;
    }
    for (std::size_t index = 0; index < keep; ++index) {
        discardFrontLocked();
    }
}

void EncodedRingBuffer::pruneByBytesLocked() {
    while (!units_.empty() && bytes_ > max_bytes_) {
        discardFrontLocked();
    }
    // A byte eviction may cut through a GOP. Remove the unusable prefix so a
    // non-empty ring begins with an independently decodable access unit.
    while (!units_.empty() && !units_.front()->independentlyDecodable()) {
        discardFrontLocked();
    }
}

void EncodedRingBuffer::discardFrontLocked() {
    bytes_ -= units_.front()->sizeBytes();
    units_.pop_front();
    if (units_.empty()) {
        generation_ = 0;
        latest_sensor_timestamp_ns_ = 0;
    }
}

}  // namespace eggvision
