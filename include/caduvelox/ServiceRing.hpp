#pragma once

#include "caduvelox/Server.hpp"
#include <thread>
#include <atomic>
#include <memory>

namespace caduvelox {

/**
 * ServiceRing: Dedicated io_uring + thread for handling connections.
 * 
 * Each ServiceRing runs on its own thread with CPU affinity pinning.
 * Connections are distributed to service rings based on fd % num_cores.
 * Each ring handles all I/O operations (TLS, recv, send, sendfile) for its assigned connections.
 * 
 * This enables parallel I/O processing across multiple cores without cross-thread contention.
 * 
 * Architecture:
 *   - 1 connection_ring (Server) for accept only
 *   - N service_rings (ServiceRing[]) for connection processing
 *   - Linear scaling: each core processes 1/N of connections independently
 */
class ServiceRing {
public:
    /**
     * Create a service ring for a specific CPU core
     * @param ring_id Unique identifier for this ring (0 to num_cores-1)
     * @param cpu_id CPU core to pin this thread to (-1 for no pinning)
     * @param queue_depth io_uring queue depth
     */
    ServiceRing(int ring_id, int cpu_id, unsigned queue_depth = 4096);
    ~ServiceRing();

    // Non-copyable, non-movable
    ServiceRing(const ServiceRing&) = delete;
    ServiceRing& operator=(const ServiceRing&) = delete;

    /**
     * Initialize the io_uring for this service ring
     */
    bool init();

    /**
     * Start the service ring thread
     */
    void start();

    /**
     * Stop the service ring thread
     */
    void stop();

    /**
     * Wait for the service ring thread to finish
     */
    void join();

    /**
     * Get the underlying Server instance (io_uring wrapper)
     */
    Server& getServer() { return server_; }

    /**
     * Get the ring ID
     */
    int getRingId() const { return ring_id_; }

    /**
     * Get the CPU ID this ring is pinned to
     */
    int getCpuId() const { return cpu_id_; }

    /**
     * Check if the ring is running
     */
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
    void run();  // Thread entry point
    void pinToCpu();  // Pin thread to CPU core

    int ring_id_;           // Unique ID for this ring (0 to N-1)
    int cpu_id_;            // CPU core to pin to (-1 for no pinning)
    unsigned queue_depth_;  // io_uring queue depth
    
    Server server_;         // Underlying io_uring server
    std::thread thread_;    // Dedicated thread for this ring
    std::atomic<bool> running_;  // Running state
};

} // namespace caduvelox
