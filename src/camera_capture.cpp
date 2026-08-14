#include "eggvision/camera_capture.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>

#include <libcamera/control_ids.h>
#include <libcamera/formats.h>
#include <libcamera/property_ids.h>

namespace eggvision {

CameraCapture::CameraCapture(const AppConfig &config, Metrics &metrics)
    : config_values_(config), metrics_(metrics), recycler_(std::make_shared<RecyclerState>()) {}

CameraCapture::~CameraCapture() {
    stop();
    unmapBuffers();
    requests_.clear();
    allocator_.reset();
    if (camera_) {
        camera_->release();
        camera_.reset();
    }
    if (manager_) {
        manager_->stop();
    }
}

void CameraCapture::setMainConsumer(FrameConsumer consumer) {
    main_consumer_ = std::move(consumer);
}

void CameraCapture::setInferenceConsumer(FrameConsumer consumer) {
    inference_consumer_ = std::move(consumer);
}

std::string CameraCapture::cameraId() const {
    return camera_ ? camera_->id() : std::string{};
}

bool CameraCapture::initialize() {
    if (initialized_.load()) {
        return true;
    }

    manager_ = std::make_unique<libcamera::CameraManager>();
    if (manager_->start() != 0) {
        std::cerr << "[camera] CameraManager start failed\n";
        return false;
    }
    if (manager_->cameras().empty()) {
        std::cerr << "[camera] no camera detected\n";
        return false;
    }

    camera_ = manager_->cameras().front();
    if (camera_->acquire() != 0) {
        std::cerr << "[camera] acquire failed: " << camera_->id() << '\n';
        return false;
    }

    if (!configureStreams() || !allocateBuffers() || !createRequests()) {
        return false;
    }

    initialized_.store(true);
    std::cout << "[camera] ready id=" << camera_->id()
              << " main=" << main_layout_.width << 'x' << main_layout_.height
              << " stride=" << main_layout_.stride
              << " lores=" << lores_layout_.width << 'x' << lores_layout_.height
              << " stride=" << lores_layout_.stride
              << " paired_requests=" << requests_.size() << '\n';
    for (std::size_t i = 0; i < main_layout_.planes.size(); ++i) {
        const auto &plane = main_layout_.planes[i];
        std::cout << "[camera] main_plane=" << i << " fd=" << plane.fd
                  << " offset=" << plane.offset << " length=" << plane.length << '\n';
    }
    return true;
}

bool CameraCapture::configureStreams() {
    camera_config_ = camera_->generateConfiguration(
        {libcamera::StreamRole::VideoRecording, libcamera::StreamRole::Viewfinder});
    if (!camera_config_ || camera_config_->size() != 2) {
        std::cerr << "[camera] camera cannot create two output streams\n";
        return false;
    }

    auto &main = camera_config_->at(0);
    main.pixelFormat = libcamera::formats::YUV420;
    main.size = libcamera::Size(config_values_.main_width, config_values_.main_height);
    main.bufferCount = config_values_.buffer_count;

    auto &lores = camera_config_->at(1);
    lores.pixelFormat = libcamera::formats::YUV420;
    lores.size = libcamera::Size(config_values_.lores_width, config_values_.lores_height);
    lores.bufferCount = config_values_.buffer_count;

    const auto status = camera_config_->validate();
    if (status == libcamera::CameraConfiguration::Invalid) {
        std::cerr << "[camera] dual-stream configuration is invalid\n";
        return false;
    }

    const bool exact = main.pixelFormat == libcamera::formats::YUV420 &&
                       lores.pixelFormat == libcamera::formats::YUV420 &&
                       main.size == libcamera::Size(config_values_.main_width, config_values_.main_height) &&
                       lores.size == libcamera::Size(config_values_.lores_width, config_values_.lores_height);
    if (!exact) {
        std::cerr << "[camera] validation adjusted outside accepted layout: main="
                  << main.toString() << " lores=" << lores.toString() << '\n';
        return false;
    }
    if (status == libcamera::CameraConfiguration::Adjusted) {
        std::cout << "[camera] configuration adjusted within accepted layout: main="
                  << main.toString() << " lores=" << lores.toString() << '\n';
    }

    if (camera_->configure(camera_config_.get()) != 0) {
        std::cerr << "[camera] configure failed\n";
        return false;
    }

    main_stream_ = main.stream();
    lores_stream_ = lores.stream();
    main_layout_.width = main.size.width;
    main_layout_.height = main.size.height;
    main_layout_.stride = main.stride;
    main_layout_.frame_size = main.frameSize;
    lores_layout_.width = lores.size.width;
    lores_layout_.height = lores.size.height;
    lores_layout_.stride = lores.stride;
    lores_layout_.frame_size = lores.frameSize;
    return true;
}

bool CameraCapture::allocateBuffers() {
    allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
    if (allocator_->allocate(main_stream_) < 0 || allocator_->allocate(lores_stream_) < 0) {
        std::cerr << "[camera] buffer allocation failed\n";
        return false;
    }

    for (const auto &buffer : allocator_->buffers(lores_stream_)) {
        std::vector<Mapping> mappings;
        mappings.reserve(buffer->planes().size());
        for (const auto &plane : buffer->planes()) {
            const std::size_t map_length = static_cast<std::size_t>(plane.offset) + plane.length;
            void *base = mmap(nullptr, map_length, PROT_READ, MAP_SHARED, plane.fd.get(), 0);
            if (base == MAP_FAILED) {
                std::cerr << "[camera] lores mmap failed: " << std::strerror(errno) << '\n';
                return false;
            }
            mappings.push_back({base,
                                map_length,
                                static_cast<const std::uint8_t *>(base) + plane.offset});
        }
        lores_mappings_.emplace(buffer.get(), std::move(mappings));
    }

    if (!allocator_->buffers(main_stream_).empty()) {
        main_layout_.planes = makeView(allocator_->buffers(main_stream_).front().get(), false).planes;
    }
    if (!allocator_->buffers(lores_stream_).empty()) {
        lores_layout_.planes = makeView(allocator_->buffers(lores_stream_).front().get(), true).planes;
    }
    return true;
}

bool CameraCapture::createRequests() {
    const auto &main_buffers = allocator_->buffers(main_stream_);
    const auto &lores_buffers = allocator_->buffers(lores_stream_);
    const std::size_t count = std::min(main_buffers.size(), lores_buffers.size());
    if (count < 4) {
        std::cerr << "[camera] fewer than four paired buffers were allocated\n";
        return false;
    }

    requests_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto request = camera_->createRequest(i);
        if (!request || request->addBuffer(main_stream_, main_buffers[i].get()) != 0 ||
            request->addBuffer(lores_stream_, lores_buffers[i].get()) != 0) {
            std::cerr << "[camera] failed to build paired request " << i << '\n';
            return false;
        }
        requests_.push_back(std::move(request));
    }
    return true;
}

bool CameraCapture::start() {
    if (!initialized_.load() || running_.load()) {
        return initialized_.load();
    }

    stopping_.store(false);
    recycler_->stopping.store(false);
    camera_->requestCompleted.connect(this, &CameraCapture::onRequestCompleted);

    libcamera::ControlList controls(camera_->controls());
    const std::int64_t frame_duration_us = 1'000'000 / config_values_.fps;
    std::array<std::int64_t, 2> duration_limits{frame_duration_us, frame_duration_us};
    controls.set(libcamera::controls::FrameDurationLimits,
                 libcamera::Span<const std::int64_t, 2>(duration_limits));
    controls.set(libcamera::controls::AeEnable, true);
    controls.set(libcamera::controls::AwbEnable, true);

    if (camera_->start(&controls) != 0) {
        camera_->requestCompleted.disconnect(this, &CameraCapture::onRequestCompleted);
        std::cerr << "[camera] start failed\n";
        return false;
    }

    running_.store(true);
    recycler_thread_ = std::thread(&CameraCapture::recyclerLoop, this);
    for (auto &request : requests_) {
        if (camera_->queueRequest(request.get()) != 0) {
            std::cerr << "[camera] initial queueRequest failed\n";
            stop();
            return false;
        }
    }
    std::cout << "[camera] capture started at " << config_values_.fps << " fps\n";
    return true;
}

void CameraCapture::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    stopping_.store(true);
    recycler_->stopping.store(true);
    recycler_->cv.notify_all();

    if (camera_) {
        camera_->stop();
        camera_->requestCompleted.disconnect(this, &CameraCapture::onRequestCompleted);
    }
    if (recycler_thread_.joinable()) {
        recycler_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(recycler_->mutex);
        recycler_->requests.clear();
    }
    std::cout << "[camera] capture stopped\n";
}

StreamView CameraCapture::makeView(libcamera::FrameBuffer *buffer, bool mapped) const {
    StreamView view = mapped ? lores_layout_ : main_layout_;
    view.planes.clear();
    const auto &planes = buffer->planes();
    const auto &metadata_planes = buffer->metadata().planes();
    const auto mapping_it = mapped ? lores_mappings_.find(buffer) : lores_mappings_.end();
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const auto &plane = planes[i];
        const std::uint32_t bytes_used = i < metadata_planes.size()
                                             ? metadata_planes[i].bytesused
                                             : plane.length;
        const std::uint8_t *data = nullptr;
        if (mapped && mapping_it != lores_mappings_.end() && i < mapping_it->second.size()) {
            data = mapping_it->second[i].data;
        }
        view.planes.push_back(
            {plane.fd.get(), plane.offset, plane.length, bytes_used, data});
    }
    return view;
}

void CameraCapture::onRequestCompleted(libcamera::Request *request) {
    if (request->status() == libcamera::Request::RequestCancelled || stopping_.load()) {
        return;
    }
    if (request->status() != libcamera::Request::RequestComplete) {
        metrics_.capture_errors.fetch_add(1);
        releaseRequest(request);
        return;
    }

    const auto main_it = request->buffers().find(main_stream_);
    const auto lores_it = request->buffers().find(lores_stream_);
    if (main_it == request->buffers().end() || lores_it == request->buffers().end()) {
        metrics_.capture_errors.fetch_add(1);
        releaseRequest(request);
        return;
    }

    auto *main_buffer = main_it->second;
    auto *lores_buffer = lores_it->second;
    const std::uint64_t sequence = main_buffer->metadata().sequence;
    const std::uint64_t timestamp = main_buffer->metadata().timestamp;
    metrics_.captured.fetch_add(1);
    metrics_.outstanding_leases.fetch_add(1);

    std::weak_ptr<RecyclerState> weak_recycler = recycler_;
    Metrics *metrics = &metrics_;
    auto lease = std::make_shared<FrameLease>(
        request,
        makeView(main_buffer, false),
        makeView(lores_buffer, true),
        sequence,
        timestamp,
        [weak_recycler, metrics](libcamera::Request *released) {
            metrics->outstanding_leases.fetch_sub(1);
            if (auto recycler = weak_recycler.lock()) {
                if (!recycler->stopping.load()) {
                    std::lock_guard<std::mutex> lock(recycler->mutex);
                    recycler->requests.push_back(released);
                    recycler->cv.notify_one();
                }
            }
        });

    try {
        if (main_consumer_) {
            main_consumer_(lease);
        }
        if (inference_consumer_) {
            inference_consumer_(lease);
        }
    } catch (const std::exception &error) {
        metrics_.capture_errors.fetch_add(1);
        std::cerr << "[camera] frame consumer failed: " << error.what() << '\n';
    }
}

void CameraCapture::releaseRequest(libcamera::Request *request) {
    if (stopping_.load() || recycler_->stopping.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(recycler_->mutex);
    recycler_->requests.push_back(request);
    recycler_->cv.notify_one();
}

void CameraCapture::recyclerLoop() {
    while (!recycler_->stopping.load()) {
        libcamera::Request *request = nullptr;
        {
            std::unique_lock<std::mutex> lock(recycler_->mutex);
            recycler_->cv.wait(lock, [this] {
                return recycler_->stopping.load() || !recycler_->requests.empty();
            });
            if (recycler_->stopping.load()) {
                break;
            }
            request = recycler_->requests.front();
            recycler_->requests.pop_front();
        }
        request->reuse(libcamera::Request::ReuseBuffers);
        if (camera_->queueRequest(request) != 0) {
            metrics_.capture_errors.fetch_add(1);
            std::cerr << "[camera] queueRequest failed while recycling\n";
        }
    }
}

void CameraCapture::unmapBuffers() {
    for (auto &[buffer, mappings] : lores_mappings_) {
        (void)buffer;
        for (auto &mapping : mappings) {
            if (mapping.base && mapping.base != MAP_FAILED) {
                munmap(mapping.base, mapping.mapped_length);
            }
        }
    }
    lores_mappings_.clear();
}

}  // namespace eggvision
