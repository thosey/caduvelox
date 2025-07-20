#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

namespace caduvelox {

// Simple thread pool for HTTP request processing
class SimpleWorkerPool {
public:
    explicit SimpleWorkerPool(size_t num_threads = std::thread::hardware_concurrency());
    ~SimpleWorkerPool();

    // Post a task to be executed on a worker thread
    void post(std::function<void()> task);

    // Shutdown the pool and wait for all threads to finish
    void shutdown();

private:
    void worker_thread();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_{false};
};

} // namespace caduvelox
