#pragma once

#include <stdexcept>
#include <string>

namespace caduvelox {

/**
 * Runtime configuration for HttpServer.
 *
 * Pass to HttpServer constructor to control resource sizing without recompiling.
 * All defaults match the previous hard-coded values so existing code is unaffected.
 *
 * Example:
 *   caduvelox::ServerConfig cfg;
 *   cfg.num_rings       = 8;
 *   cfg.queue_depth     = 2048;
 *   cfg.ktls_pool_size  = 128;
 *   caduvelox::HttpServer server(cfg);
 */
struct ServerConfig {
    // io_uring event loop
    unsigned queue_depth               = 4096;   // SQE/CQE ring depth per service ring

    // Zero-copy recv buffer ring (allocated once per ring at startup)
    unsigned buffer_ring_count         = 512;    // Number of pre-allocated recv buffers
    unsigned buffer_size_bytes         = 16384;  // Size of each recv buffer (16 KB)

    // Job pool sizes (per ring thread — no cross-thread contention)
    unsigned ktls_pool_size            = 5000;   // Max concurrent TLS handshakes
    unsigned accept_pool_size          = 2;      // One live AcceptJob per ring; 2 covers error-recovery overlap
    unsigned write_pool_size           = 10000;  // Max concurrent write operations
    unsigned file_job_pool_size        = 1000;   // Max concurrent file transfer operations
    unsigned connection_pool_size      = 10000;  // Max concurrent HTTP connections

    // Timeouts
    unsigned ktls_handshake_timeout_ms = 5000;   // Per-step TLS handshake timeout (ms)
    unsigned idle_timeout_ms           = 60000;  // Keep-alive idle timeout (ms); 0 = disabled

    // Threading
    int num_rings = 0;  // 0 = auto-detect via hardware_concurrency()

    void validate() const {
        auto check = [](unsigned v, const char* name) {
            if (v == 0)
                throw std::invalid_argument(
                    std::string("ServerConfig: ") + name + " must be > 0");
        };
        check(ktls_pool_size,       "ktls_pool_size");
        check(accept_pool_size,     "accept_pool_size");
        check(write_pool_size,      "write_pool_size");
        check(file_job_pool_size,   "file_job_pool_size");
        check(connection_pool_size, "connection_pool_size");
    }
};

} // namespace caduvelox
