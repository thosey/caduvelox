#pragma once
#include <thread>
#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <memory>
#include "caduvelox/ring_buffer/VyukovRingBuffer.hpp"

namespace caduvelox {

// Connection-affinity worker pool where each connection is tied to a specific worker thread
// This eliminates data races with multishot operations by ensuring all processing for a 
// connection happens on the same thread in order.
class AffinityWorkerPool {
public:
    explicit AffinityWorkerPool(size_t num_threads = std::thread::hardware_concurrency());
    ~AffinityWorkerPool();

    // Post a task for a specific connection (connection_id determines worker thread)
    void post(int connection_id, std::function<void()> task);
    
    // Post a generic task to any worker (for non-connection-specific work)
    void post_any(std::function<void()> task);

    // Shutdown the pool and wait for all threads to finish
    void shutdown();

    // Get the worker thread ID that will handle a specific connection
    size_t get_worker_for_connection(int connection_id) const;

private:
    void worker_thread(size_t worker_id);
    
    struct TaskItem {
        std::function<void()> task;
        TaskItem() = default;
        TaskItem(std::function<void()> t) : task(std::move(t)) {}
    };

    std::vector<std::thread> workers_;
    
    // Each worker has its own lock-free queue to avoid contention
    // Using compile-time size for VyukovRingBuffer
    static constexpr size_t QUEUE_SIZE = 1024;
    std::vector<std::unique_ptr<VyukovRingBuffer<TaskItem, QUEUE_SIZE>>> worker_queues_;
    
    // Round-robin counter for post_any
    std::atomic<size_t> round_robin_counter_{0};
    
    std::atomic<bool> stop_{false};
    size_t num_threads_;
};

} // namespace caduvelox
