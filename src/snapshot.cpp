#include "eggvision/snapshot.hpp"
#include "eggvision/dma_buf_sync.hpp"

#include <utility>

namespace eggvision {

bool stageMainSnapshot(const FrameLease &frame, MainSnapshot &snapshot, std::string &error) {
    error.clear();
    snapshot = {};
    const StreamView &view = frame.main();
    for (const PlaneView &plane : view.planes) {
        if (!plane.data) {
            error = "main profile is not CPU mapped";
            return false;
        }
    }
    DmaBufReadSync read_sync(view, error);
    if (!read_sync) {
        return false;
    }

    const bool copied = copyMappedI420(view, snapshot.i420, error);
    std::string sync_error;
    const bool ended = read_sync.finish(sync_error);
    if (!ended && error.empty()) {
        error = std::move(sync_error);
    }
    if (!copied || !ended) {
        snapshot.i420.clear();
        return false;
    }

    snapshot.width = view.width;
    snapshot.height = view.height;
    snapshot.sequence = frame.sequence();
    snapshot.sensor_timestamp_ns = frame.sensorTimestampNs();
    return true;
}

}  // namespace eggvision
