#pragma once

#include "eggvision/encoded_access_unit.hpp"
#include "eggvision/inference.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace eggvision {

class AppConfig;
class EncodedRingBuffer;
class FrameLease;
struct Metrics;

enum class EventState {
    Idle,
    CollectingPostRoll,
    Cooldown,
};

// Small deterministic state machine kept separate from I/O so suppression
// boundaries can be unit tested with sensor timestamps.
class EventGate {
public:
    explicit EventGate(std::uint64_t cooldown_ns);

    bool tryBegin(std::uint64_t trigger_sensor_timestamp_ns);
    void complete(std::uint64_t actual_end_sensor_timestamp_ns);
    EventState stateAt(std::uint64_t sensor_timestamp_ns) const;
    std::uint64_t cooldownUntilNs() const { return cooldown_until_ns_; }

private:
    std::uint64_t cooldown_ns_ = 0;
    std::uint64_t cooldown_until_ns_ = 0;
    bool collecting_ = false;
};

// Coordinates a single detection event. Camera leases are copied during
// trigger() and are never retained by the asynchronous JPEG/mux worker.
class EventRecorder {
public:
    EventRecorder(const AppConfig &config, EncodedRingBuffer &history, Metrics &metrics);
    ~EventRecorder();

    EventRecorder(const EventRecorder &) = delete;
    EventRecorder &operator=(const EventRecorder &) = delete;

    bool initialize();
    bool start();
    bool trigger(const std::shared_ptr<FrameLease> &frame,
                 const std::vector<Detection> &detections);
    void observeEncoded(const EncodedAccessUnitPtr &unit);
    void stop();
    EventState state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eggvision
