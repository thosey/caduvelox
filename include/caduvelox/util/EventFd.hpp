#pragma once

#include <sys/eventfd.h>
#include <unistd.h>
#include <cstdint>
#include <system_error>

namespace caduvelox {

/**
 * RAII wrapper for Linux eventfd
 * Used for cross-thread notification (worker threads signal io_uring thread)
 * 
 * eventfd advantages:
 * - Integrates with io_uring (can be polled via IORING_OP_READ)
 * - Lightweight (just a counter, no buffers)
 * - Multiple writes coalesce into single read
 */
class EventFd {
public:
    /**
     * Create an eventfd
     * @param semaphore If true, reads return 1 and decrement counter by 1 (semaphore mode)
     *                  If false, reads return counter value and reset to 0
     * @param nonblocking If true, read/write operations don't block
     */
    explicit EventFd(bool semaphore = false, bool nonblocking = true) {
        int flags = EFD_CLOEXEC;
        if (semaphore) flags |= EFD_SEMAPHORE;
        if (nonblocking) flags |= EFD_NONBLOCK;
        
        fd_ = eventfd(0, flags);
        if (fd_ < 0) {
            throw std::system_error(errno, std::system_category(), "eventfd creation failed");
        }
    }
    
    ~EventFd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    
    // Non-copyable
    EventFd(const EventFd&) = delete;
    EventFd& operator=(const EventFd&) = delete;
    
    // Movable
    EventFd(EventFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    
    EventFd& operator=(EventFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    
    /**
     * Get the file descriptor (for use with io_uring or poll)
     */
    int fd() const { return fd_; }
    
    /**
     * Signal the eventfd (increment counter by 1)
     * Called by producer (worker thread) to wake up consumer (io_uring thread)
     */
    void signal() {
        uint64_t value = 1;
        ssize_t result = write(fd_, &value, sizeof(value));
        // In nonblocking mode, EAGAIN means already signaled (acceptable)
        if (result < 0 && errno != EAGAIN) {
            throw std::system_error(errno, std::system_category(), "eventfd write failed");
        }
    }
    
    /**
     * Read/consume the eventfd counter
     * Called by consumer (io_uring thread) after waking up
     * Returns the counter value (or 1 in semaphore mode)
     */
    uint64_t consume() {
        uint64_t value = 0;
        ssize_t result = read(fd_, &value, sizeof(value));
        if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;  // No signal pending
            }
            throw std::system_error(errno, std::system_category(), "eventfd read failed");
        }
        return value;
    }
    
    /**
     * Try to consume without blocking
     * Returns 0 if no signals pending
     */
    uint64_t try_consume() {
        uint64_t value = 0;
        ssize_t result = read(fd_, &value, sizeof(value));
        if (result < 0) {
            return 0;  // EAGAIN or other error
        }
        return value;
    }
    
private:
    int fd_;
};

} // namespace caduvelox
