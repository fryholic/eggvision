#pragma once

#include <atomic>
#include <cstdint>

namespace eggvision {

struct Metrics {
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> capture_errors{0};
    std::atomic<std::uint64_t> outstanding_leases{0};
    std::atomic<std::uint64_t> encoder_access_units{0};
    std::atomic<std::uint64_t> encoder_output_bytes{0};
    std::atomic<std::uint64_t> encoder_dropped{0};
    std::atomic<std::uint64_t> encoder_errors{0};
    std::atomic<std::uint64_t> rtsp_pushed{0};
    std::atomic<std::uint64_t> rtsp_dropped{0};
    std::atomic<std::uint64_t> rtsp_errors{0};
    std::atomic<std::uint64_t> rtsp_recoveries{0};
    std::atomic<std::uint64_t> rtsp_recovery_failures{0};
    std::atomic<std::uint64_t> rtsp_sessions_current{0};
    std::atomic<std::uint64_t> rtsp_sessions_peak{0};
    std::atomic<std::uint64_t> rtsp_sessions_cleaned{0};
    std::atomic<std::uint64_t> inference_processed{0};
    std::atomic<std::uint64_t> inference_dropped{0};
    std::atomic<std::uint64_t> detected_persons{0};
    std::atomic<std::uint64_t> inference_total_us{0};
    std::atomic<std::uint64_t> events_triggered{0};
    std::atomic<std::uint64_t> events_suppressed{0};
    std::atomic<std::uint64_t> events_completed{0};
    std::atomic<std::uint64_t> events_partial_preroll{0};
    std::atomic<std::uint64_t> events_partial_postroll{0};
    std::atomic<std::uint64_t> events_failed{0};
    std::atomic<std::uint64_t> event_video_bytes{0};
    std::atomic<std::uint64_t> event_snapshot_bytes{0};
    std::atomic<std::uint64_t> event_mux_errors{0};
    std::atomic<std::uint64_t> event_snapshot_errors{0};
    std::atomic<std::uint64_t> event_disk_space_rejections{0};
};

}  // namespace eggvision
