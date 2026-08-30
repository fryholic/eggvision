#pragma once

#include <ios>
#include <ostream>
#include <sstream>
#include <utility>

namespace eggvision {

class SynchronizedLogLine {
public:
    explicit SynchronizedLogLine(std::ostream &stream) noexcept;
    ~SynchronizedLogLine() noexcept;

    SynchronizedLogLine(const SynchronizedLogLine &) = delete;
    SynchronizedLogLine &operator=(const SynchronizedLogLine &) = delete;
    SynchronizedLogLine(SynchronizedLogLine &&) = delete;
    SynchronizedLogLine &operator=(SynchronizedLogLine &&) = delete;

    template <typename Value>
    SynchronizedLogLine &operator<<(Value &&value) {
        buffer_ << std::forward<Value>(value);
        return *this;
    }

    SynchronizedLogLine &operator<<(std::ostream &(*manipulator)(std::ostream &));
    SynchronizedLogLine &operator<<(std::ios_base &(*manipulator)(std::ios_base &));

private:
    std::ostream &stream_;
    std::ostringstream buffer_;
};

SynchronizedLogLine synchronizedLog(std::ostream &stream) noexcept;

}  // namespace eggvision
