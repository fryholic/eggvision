#pragma once

#include "eggvision/config.hpp"
#include "eggvision/frame.hpp"
#include "eggvision/metrics.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/framebuffer_allocator.h>

namespace eggvision {

class CameraCapture {
public:
    using FrameConsumer = std::function<void(std::shared_ptr<FrameLease>)>;

    CameraCapture(const AppConfig &config, Metrics &metrics);
    ~CameraCapture();

    CameraCapture(const CameraCapture &) = delete;
    CameraCapture &operator=(const CameraCapture &) = delete;

    void setMainConsumer(FrameConsumer consumer);
    void setInferenceConsumer(FrameConsumer consumer);

    bool initialize();
    bool start();
    void stop();

    std::string cameraId() const;
    const StreamView &mainLayout() const { return main_layout_; }
    const StreamView &loresLayout() const { return lores_layout_; }

private:
    struct Mapping {
        void *base = nullptr;
        std::size_t mapped_length = 0;
        const std::uint8_t *data = nullptr;
    };

    using MappingTable =
        std::unordered_map<libcamera::FrameBuffer *, std::vector<Mapping>>;

    struct RecyclerState {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<libcamera::Request *> requests;
        std::atomic<bool> stopping{false};
    };

    bool configureStreams();
    bool allocateBuffers();
    bool createRequests();
    bool mapBuffers(libcamera::Stream *stream, MappingTable &mappings, const char *name);
    static void unmapBuffers(MappingTable &mappings);
    void onRequestCompleted(libcamera::Request *request);
    void recyclerLoop();
    StreamView makeView(libcamera::FrameBuffer *buffer,
                        const StreamView &layout,
                        const MappingTable &mappings) const;
    void releaseRequest(libcamera::Request *request);

    AppConfig config_values_;
    Metrics &metrics_;
    std::unique_ptr<libcamera::CameraManager> manager_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> camera_config_;
    libcamera::Stream *main_stream_ = nullptr;
    libcamera::Stream *lores_stream_ = nullptr;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    MappingTable main_mappings_;
    MappingTable lores_mappings_;
    std::shared_ptr<RecyclerState> recycler_;
    std::thread recycler_thread_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    FrameConsumer main_consumer_;
    FrameConsumer inference_consumer_;
    StreamView main_layout_;
    StreamView lores_layout_;
};

}  // namespace eggvision

