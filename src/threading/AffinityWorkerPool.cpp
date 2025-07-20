#include "caduvelox/threading/AffinityWorkerPool.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <chrono>

namespace caduvelox {

AffinityWorkerPool::AffinityWorkerPool(size_t num_threads) 
    : num_threads_(num_threads) {
    
    // Create per-worker lock-free queues
    worker_queues_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        worker_queues_.emplace_back(std::make_unique<VyukovRingBuffer<TaskItem, QUEUE_SIZE>>());
    }
    
    // Start worker threads
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&AffinityWorkerPool::worker_thread, this, i);
    }
}

AffinityWorkerPool::~AffinityWorkerPool() {
    shutdown();
}

void AffinityWorkerPool::post(int connection_id, std::function<void()> task) {
    if (stop_.load()) return;
    
    // Hash connection_id to determine worker thread
    size_t worker_id = static_cast<size_t>(connection_id) % num_threads_;
    
    TaskItem item(std::move(task));
    if (!worker_queues_[worker_id]->enqueue(std::move(item))) {
        // Queue full - log error about dropped task
        Logger::getInstance().logError("AffinityWorkerPool: Queue full for worker " + 
                                       std::to_string(worker_id) + ", dropping task for connection " + 
                                       std::to_string(connection_id));
    }
}

void AffinityWorkerPool::post_any(std::function<void()> task) {
    if (stop_.load()) return;
    
    // Round-robin to distribute generic tasks evenly
    size_t worker_id = round_robin_counter_.fetch_add(1) % num_threads_;
    
    TaskItem item(std::move(task));
    if (!worker_queues_[worker_id]->enqueue(std::move(item))) {
        // Queue full - try next worker
        for (size_t i = 1; i < num_threads_; ++i) {
            size_t next_worker = (worker_id + i) % num_threads_;
            if (worker_queues_[next_worker]->enqueue(std::move(item))) {
                return;
            }
        }
        // All queues full - log and drop task
        Logger::getInstance().logError("AffinityWorkerPool: All worker queues full, dropping task");
    }
}

void AffinityWorkerPool::shutdown() {
    stop_.store(true);
    
    // Workers will exit when they see stop_ == true
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

size_t AffinityWorkerPool::get_worker_for_connection(int connection_id) const {
    return static_cast<size_t>(connection_id) % num_threads_;
}

void AffinityWorkerPool::worker_thread(size_t worker_id) {
    auto& queue = *worker_queues_[worker_id];
    
    while (!stop_.load()) {
        TaskItem item;
        
        // Try to dequeue a task
        if (queue.dequeue(item)) {
            if (item.task) {
                try {
                    item.task();
                } catch (...) {
                    // Log error but continue processing
                    // Could add error callback here
                }
            }
        } else {
            // No tasks available, sleep briefly to avoid busy waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    // Process remaining tasks during shutdown
    TaskItem item;
    while (queue.dequeue(item)) {
        if (item.task) {
            try {
                item.task();
            } catch (...) {
                // Ignore errors during shutdown
            }
        }
    }
}

} // namespace caduvelox
