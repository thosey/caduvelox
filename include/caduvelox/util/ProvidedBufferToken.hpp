#pragma once

#include "caduvelox/ring_buffer/BufferRingCoordinator.hpp"
#include <cstddef>

namespace caduvelox {

/**
 * RAII wrapper for io_uring provided buffers that automatically recycles
 * the buffer when processing is complete. Enables zero-copy data processing
 * by passing provided buffers directly for inline processing.
 */
class ProvidedBufferToken {
public:
    ProvidedBufferToken(BufferRingCoordinator* coordinator, unsigned buf_id, char* data, size_t size)
        : coordinator_(coordinator), buf_id_(buf_id), data_(data), size_(size) {}

    ~ProvidedBufferToken() {
        if (data_ && coordinator_) {
            coordinator_->recycleBuffer(buf_id_);
        }
    }

    const char* data() const { return data_; }
    size_t size() const { return size_; }
    unsigned buffer_id() const { return buf_id_; }

    // Non-copyable to prevent accidental duplication and double-recycling
    ProvidedBufferToken(const ProvidedBufferToken&) = delete;
    ProvidedBufferToken& operator=(const ProvidedBufferToken&) = delete;

    // Movable for efficient transfer
    ProvidedBufferToken(ProvidedBufferToken&& other) noexcept
        : coordinator_(other.coordinator_), buf_id_(other.buf_id_),
          data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
    }

    ProvidedBufferToken& operator=(ProvidedBufferToken&& other) noexcept {
        if (this != &other) {
            if (data_ && coordinator_) {
                coordinator_->recycleBuffer(buf_id_);
            }
            coordinator_ = other.coordinator_;
            buf_id_ = other.buf_id_;
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
        }
        return *this;
    }

    // Check if token is valid (not moved-from)
    bool valid() const { return data_ != nullptr; }

    // Manually invalidate the token to prevent recycling in destructor
    void invalidate() { data_ = nullptr; }

private:
    BufferRingCoordinator* coordinator_;
    unsigned buf_id_;
    char* data_;
    size_t size_;
};

} // namespace caduvelox
