#pragma once

#include <memory>
#include <functional>

namespace caduvelox {

/**
 * RAII wrapper for io_uring provided buffers that automatically recycles
 * the buffer when processing is complete. Enables zero-copy data processing
 * by passing provided buffers directly to worker threads.
 */
class ProvidedBufferToken {
public:
    using RecycleCallback = std::function<void(unsigned)>;
    
    ProvidedBufferToken(RecycleCallback recycle_cb, unsigned buf_id, char* data, size_t size)
        : recycle_cb_(std::move(recycle_cb)), buf_id_(buf_id), data_(data), size_(size) {}
    
    ~ProvidedBufferToken() {
        // Automatically recycle buffer when processing is done
        if (data_ && recycle_cb_) {
            recycle_cb_(buf_id_);
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
        : recycle_cb_(std::move(other.recycle_cb_)), buf_id_(other.buf_id_), 
          data_(other.data_), size_(other.size_) {
        other.data_ = nullptr; // Mark as moved
    }
    
    ProvidedBufferToken& operator=(ProvidedBufferToken&& other) noexcept {
        if (this != &other) {
            // Recycle current buffer if valid
            if (data_ && recycle_cb_) {
                recycle_cb_(buf_id_);
            }
            
            recycle_cb_ = std::move(other.recycle_cb_);
            buf_id_ = other.buf_id_;
            data_ = other.data_;
            size_ = other.size_;
            
            other.data_ = nullptr; // Mark as moved
        }
        return *this;
    }
    
    // Check if token is valid (not moved-from)
    bool valid() const { return data_ != nullptr; }
    
    // Manually invalidate the token to prevent recycling in destructor
    void invalidate() { data_ = nullptr; }

private:
    RecycleCallback recycle_cb_;
    unsigned buf_id_;
    char* data_;
    size_t size_;
};

} // namespace caduvelox
