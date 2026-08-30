#include "eggvision/logging.hpp"

#include <mutex>

namespace eggvision {
namespace {

std::mutex output_mutex;

}  // namespace

SynchronizedLogLine::SynchronizedLogLine(std::ostream &stream) noexcept : stream_(stream) {}

SynchronizedLogLine::~SynchronizedLogLine() noexcept {
    try {
        const std::string text = buffer_.str();
        std::lock_guard<std::mutex> lock(output_mutex);
        stream_ << text;
        stream_.flush();
    } catch (...) {
        // Logging must never unwind across worker or C callback boundaries.
    }
}

SynchronizedLogLine &SynchronizedLogLine::operator<<(
    std::ostream &(*manipulator)(std::ostream &)) {
    manipulator(buffer_);
    return *this;
}

SynchronizedLogLine &SynchronizedLogLine::operator<<(
    std::ios_base &(*manipulator)(std::ios_base &)) {
    manipulator(buffer_);
    return *this;
}

SynchronizedLogLine synchronizedLog(std::ostream &stream) noexcept {
    return SynchronizedLogLine(stream);
}

}  // namespace eggvision
