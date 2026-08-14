#pragma once

#include "eggvision/encoded_access_unit.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace eggvision {

enum class EncodedRingPushResult {
    Accepted,
    ResetForGeneration,
    ResetForTimestampRegression,
    RejectedInvalid,
    RejectedOversize,
};

struct EncodedRingSelection {
    std::uint64_t generation = 0;
    std::uint64_t requested_start_sensor_timestamp_ns = 0;
    std::uint64_t actual_start_sensor_timestamp_ns = 0;
    bool pre_roll_complete = false;
    bool has_independent_start = false;
    std::vector<EncodedAccessUnitPtr> units;
};

struct EncodedRingStats {
    std::size_t units = 0;
    std::size_t bytes = 0;
    std::size_t peak_bytes = 0;
    std::uint64_t resets = 0;
};

// A bounded, thread-safe history of compressed H.264 access units. Retention
// is constrained by both sensor time and payload bytes. The ring never mixes
// encoder generations or timestamps that move backwards.
class EncodedRingBuffer {
public:
    EncodedRingBuffer(std::uint64_t retention_ns, std::size_t max_bytes);

    EncodedRingPushResult push(EncodedAccessUnitPtr unit);
    EncodedRingSelection selectPreRoll(std::uint64_t trigger_sensor_timestamp_ns,
                                       std::uint64_t pre_roll_ns) const;
    void clear();
    EncodedRingStats stats() const;

    std::uint64_t retentionNs() const { return retention_ns_; }
    std::size_t maxBytes() const { return max_bytes_; }

private:
    void resetLocked();
    void pruneByTimeLocked();
    void pruneByBytesLocked();
    void discardFrontLocked();

    const std::uint64_t retention_ns_;
    const std::size_t max_bytes_;
    mutable std::mutex mutex_;
    std::deque<EncodedAccessUnitPtr> units_;
    std::size_t bytes_ = 0;
    std::size_t peak_bytes_ = 0;
    std::uint64_t resets_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t latest_sensor_timestamp_ns_ = 0;
};

}  // namespace eggvision
