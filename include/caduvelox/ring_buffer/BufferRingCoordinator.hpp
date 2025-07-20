#pragma once

#include <liburing.h>
#include <memory>

namespace caduvelox {

class Logger;

/**
 * Manages io_uring provided buffer rings for zero-copy recv operations.
 * Handles buffer allocation, recycling, and ring management lifecycle.
 */
class BufferRingCoordinator {
public:
    static constexpr unsigned DEFAULT_BUF_COUNT = 512;
    static constexpr int DEFAULT_BUFFER_SIZE = 16384; // 16KB for recv operations
    static constexpr unsigned DEFAULT_BUF_GROUP_ID = 1;

    explicit BufferRingCoordinator(
        unsigned buf_count = DEFAULT_BUF_COUNT,
        size_t buf_size = DEFAULT_BUFFER_SIZE,
        unsigned buf_group_id = DEFAULT_BUF_GROUP_ID
    );
    
    ~BufferRingCoordinator();

    // Buffer ring lifecycle
    bool setupBufferRing(struct io_uring* ring);
    void cleanupBufferRing();
    
    // Buffer ring queries
    bool hasBufferRing() const;
    unsigned getBufferGroupId() const;
    struct io_uring_buf_ring* getBufferRing();
    
    // Buffer operations
    void* getBufferPtr(unsigned buffer_id) const;
    size_t getBufferSize() const;
    void recycleBuffer(unsigned buffer_id);
    
    // Configuration
    unsigned getBufferCount() const { return buf_count_; }

private:
    // Buffer ring configuration
    unsigned buf_count_;
    size_t buf_size_;
    unsigned buf_group_id_;
    
    // Buffer ring state
    struct io_uring_buf_ring* buffer_ring_{nullptr};
    void* buffer_block_{nullptr};
    unsigned buffer_ring_mask_{0};
    
    Logger& logger_;
};

} // namespace caduvelox
