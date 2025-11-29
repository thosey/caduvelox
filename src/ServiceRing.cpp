#include "caduvelox/ServiceRing.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <pthread.h>
#include <sched.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace caduvelox {

ServiceRing::ServiceRing(int ring_id, int cpu_id, unsigned queue_depth)
    : ring_id_(ring_id), cpu_id_(cpu_id), queue_depth_(queue_depth),
      server_(), running_(false) {
}

ServiceRing::~ServiceRing() {
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool ServiceRing::init() {
    // Initialize the underlying io_uring server
    try {
        if (!server_.init(queue_depth_)) {
            Logger::getInstance().logError("ServiceRing[" + std::to_string(ring_id_) + 
                                          "]: Failed to initialize io_uring");
            return false;
        }
        
        Logger::getInstance().logMessage("ServiceRing[" + std::to_string(ring_id_) + 
                                        "]: Initialized with queue depth " + 
                                        std::to_string(queue_depth_) + 
                                        (cpu_id_ >= 0 ? " (pinned to CPU " + std::to_string(cpu_id_) + ")" : ""));
        return true;
    } catch (const std::exception& e) {
        Logger::getInstance().logError("ServiceRing[" + std::to_string(ring_id_) + 
                                      "]: Exception during init: " + std::string(e.what()));
        return false;
    }
}

void ServiceRing::start() {
    if (running_.load(std::memory_order_acquire)) {
        Logger::getInstance().logMessage("ServiceRing[" + std::to_string(ring_id_) + 
                                        "]: Already running");
        return;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ServiceRing::run, this);
    
    Logger::getInstance().logMessage("ServiceRing[" + std::to_string(ring_id_) + 
                                    "]: Thread started");
}

void ServiceRing::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);
    server_.stop();  // Wake up the io_uring event loop
    
    Logger::getInstance().logMessage("ServiceRing[" + std::to_string(ring_id_) + 
                                    "]: Stop requested");
}

void ServiceRing::join() {
    if (thread_.joinable()) {
        thread_.join();
        Logger::getInstance().logMessage("ServiceRing[" + std::to_string(ring_id_) + 
                                        "]: Thread joined");
    }
}

void ServiceRing::run() {
    // Pin to CPU core if specified
    if (cpu_id_ >= 0) {
        pinToCpu();
    }

    Logger::getInstance().logMessage("ServiceRing[" + std::to_string(ring_id_) + 
                                    "]: Event loop starting on CPU " + 
                                    std::to_string(sched_getcpu()));

    // Run the io_uring event loop (blocking)
    server_.run();

    Logger::getInstance().logMessage("ServiceRing[" + std::to_string(ring_id_) + 
                                    "]: Event loop stopped");
}

void ServiceRing::pinToCpu() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id_, &cpuset);

    pthread_t thread = pthread_self();
    int ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    
    if (ret != 0) {
        Logger::getInstance().logError("ServiceRing[" + std::to_string(ring_id_) + 
                                      "]: Failed to pin to CPU " + std::to_string(cpu_id_) + 
                                      ": " + std::string(strerror(ret)));
    } else {
        Logger::getInstance().logMessage("ServiceRing[" + std::to_string(ring_id_) + 
                                        "]: Successfully pinned to CPU " + std::to_string(cpu_id_));
    }
}

} // namespace caduvelox
