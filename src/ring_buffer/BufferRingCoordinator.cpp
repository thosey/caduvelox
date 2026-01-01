/*
 * BufferRingCoordinator: Manages io_uring provided buffer rings for zero-copy recv operations
 */

#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <sys/mman.h>
#include <liburing.h>
#include <cstring>
#include <iostream>

namespace caduvelox {

BufferRingCoordinator::BufferRingCoordinator(
    unsigned buf_count,
    size_t buf_size,
    unsigned buf_group_id
) : buf_count_(buf_count),
    buf_size_(buf_size),
    buf_group_id_(buf_group_id),
    logger_(Logger::getInstance()) {
}

BufferRingCoordinator::~BufferRingCoordinator() {
    cleanupBufferRing();
}

bool BufferRingCoordinator::setupBufferRing(struct io_uring* ring) {
    if (buffer_ring_) {
        logger_.logMessage("Buffer ring already set up");
        return true;
    }

    // Allocate memory for all buffers in a single mmap block
    size_t total_size = buf_count_ * buf_size_;
    buffer_block_ = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, 
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buffer_block_ == MAP_FAILED) {
        logger_.logError("Failed to allocate buffer block: " + std::string(strerror(errno)));
        buffer_block_ = nullptr;
        return false;
    }

    // Compute ring mask from number of entries
    buffer_ring_mask_ = io_uring_buf_ring_mask(buf_count_);

    // Register the provided buffer ring with liburing
    int err = 0;
    buffer_ring_ = io_uring_setup_buf_ring(ring, buf_count_, buf_group_id_, 0, &err);
    if (!buffer_ring_ || err) {
        logger_.logError("Failed to setup buffer ring: " + std::string(strerror(err)));
        cleanupBufferRing();
        return false;
    }

    logger_.logMessage("Buffer ring created successfully: ring=" + 
                      std::to_string(reinterpret_cast<uintptr_t>(buffer_ring_)) + 
                      " group_id=" + std::to_string(buf_group_id_));

    // Populate the ring with buffers 
    // Add all buffers first, then advance once by the total count
    for (unsigned i = 0; i < buf_count_; ++i) {
        void* base = static_cast<char*>(buffer_block_) + i * buf_size_;
        // buf_offset is the position relative to current tail
        io_uring_buf_ring_add(buffer_ring_, base, buf_size_, i, buffer_ring_mask_, i);
    }
    // Advance once by the total count to make all buffers visible
    io_uring_buf_ring_advance(buffer_ring_, buf_count_);

    logger_.logMessage("Buffer ring setup complete: " + std::to_string(buf_count_) + 
                      " buffers of " + std::to_string(buf_size_) + " bytes each, " +
                      "attempting to start allocation from buffer 0");
    return true;
}

void BufferRingCoordinator::cleanupBufferRing() {
    if (buffer_block_) {
        size_t total_size = buf_count_ * buf_size_;
        ::munmap(buffer_block_, total_size);
        buffer_block_ = nullptr;
    }
    
    // Note: buffer_ring_ is managed by liburing, don't free it manually
    buffer_ring_ = nullptr;
    buf_count_ = 0;
}

bool BufferRingCoordinator::hasBufferRing() const {
    return buffer_ring_ != nullptr;
}

unsigned BufferRingCoordinator::getBufferGroupId() const {
    return buf_group_id_;
}

struct io_uring_buf_ring* BufferRingCoordinator::getBufferRing() {
    return buffer_ring_;
}

void* BufferRingCoordinator::getBufferPtr(unsigned buffer_id) const {
    if (!buffer_block_ || buffer_id >= buf_count_) {
        return nullptr;
    }
    return static_cast<char*>(buffer_block_) + buffer_id * buf_size_;
}

size_t BufferRingCoordinator::getBufferSize() const {
    return buf_size_;
}

void BufferRingCoordinator::recycleBuffer(unsigned buffer_id) {
    if (!buffer_ring_ || !buffer_block_) {
        logger_.logError("Cannot recycle buffer: ring or block is null");
        return;
    }
    
    void* base = getBufferPtr(buffer_id);
    if (!base) {
        logger_.logError("Invalid buffer ID for recycling: " + std::to_string(buffer_id));
        return;
    }
    
    // Recycle the buffer back to the available pool
    // Use the buffer_id as the position to maintain natural order
    io_uring_buf_ring_add(buffer_ring_, base, buf_size_, buffer_id, 
                         buffer_ring_mask_, 0);
    io_uring_buf_ring_advance(buffer_ring_, 1);
    
    logger_.logMessage("Recycled buffer " + std::to_string(buffer_id) + 
                      " back to available pool (natural order preserved)");
}

} // namespace caduvelox
