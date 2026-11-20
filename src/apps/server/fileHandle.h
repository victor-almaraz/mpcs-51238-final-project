// A POSIX file descriptor, closed with its owner: the bridges' devices,
// listeners, and connections.

#pragma once

#include <utility>

#include <unistd.h>

namespace server {

/// Owns a file descriptor; -1 holds none.
class fileHandle {
public:
    fileHandle() noexcept = default;
    explicit fileHandle(int fd) noexcept : fd_(fd) {}
    ~fileHandle() { reset(); }

    fileHandle(const fileHandle&) = delete;
    fileHandle& operator=(const fileHandle&) = delete;
    fileHandle(fileHandle&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    fileHandle& operator=(fileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    /// The descriptor, or -1.
    int get() const noexcept { return fd_; }
    /// True when it holds a descriptor.
    explicit operator bool() const noexcept { return fd_ >= 0; }
    /// Closes the descriptor it holds.
    void reset() noexcept {
        if (fd_ >= 0) ::close(std::exchange(fd_, -1));
    }

private:
    int fd_ = -1;
};

}  // namespace server
