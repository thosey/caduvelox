#include "caduvelox/threading/SimpleWorkerPool.hpp"

namespace caduvelox {

SimpleWorkerPool::SimpleWorkerPool(size_t num_threads) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&SimpleWorkerPool::worker_thread, this);
    }
}

SimpleWorkerPool::~SimpleWorkerPool() {
    shutdown();
}

void SimpleWorkerPool::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stop_) return; // Don't accept new tasks if shutting down
        tasks_.emplace(std::move(task));
    }
    condition_.notify_one();
}

void SimpleWorkerPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void SimpleWorkerPool::worker_thread() {
    while (true) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            
            if (stop_ && tasks_.empty()) {
                break;
            }
            
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
        }
        
        if (task) {
            task();
        }
    }
}

} // namespace caduvelox
