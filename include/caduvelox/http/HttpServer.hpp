#pragma once

#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/ServerState.hpp"
#include "caduvelox/ServiceRing.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/http/HttpRouter.hpp"
#include <atomic>
#include <vector>
#include <memory>
#include <string>

namespace caduvelox {

/**
 * HttpServer: High-performance HTTPS server with optimized multi-threaded architecture.
 * 
 * This server uses a per-core threading model with SO_REUSEPORT for maximum performance:
 * 
 * Architecture:
 *   - N service rings (one per CPU core by default)
 *   - Each ring has:
 *     * Dedicated io_uring + thread pinned to CPU core
 *     * Own listening socket (SO_REUSEPORT for kernel load balancing)
 *     * Inline HTTP processing (no worker thread overhead)
 *   - Kernel distributes incoming connections across sockets
 * 
 * Usage:
 *   HttpServer server(num_cores);  // or 0 for auto-detect
 *   server.addRoute("GET", "/", handler);
 *   server.listenKTLS(8443, "cert.pem", "key.pem");
 *   server.run();  // Blocking
 */
class HttpServer {
public:
    /**
     * Create HTTPS server from a ServerConfig.
     * All resource sizes (pool capacities, buffer ring, queue depth) are taken from cfg.
     */
    explicit HttpServer(const ServerConfig& cfg = ServerConfig{});

    /**
     * Convenience constructor — equivalent to ServerConfig{.num_rings=num_rings, .queue_depth=queue_depth}.
     */
    explicit HttpServer(int num_rings, unsigned queue_depth = 4096);
    ~HttpServer();

    // Non-copyable, non-movable
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /**
     * Add a route to all HttpServer instances
     * Must be called before listenKTLS()
     */
    void addRoute(const std::string& method, const std::string& path_pattern,
                  HttpHandler handler);

    /**
     * Start HTTPS server with KTLS on specified port
     * Creates listening socket and starts accepting connections
     * @return true on success, false on failure
     */
    bool listenKTLS(int port, const std::string& cert_path, const std::string& key_path,
                    const std::string& bind_addr = "0.0.0.0");

    /**
     * Run the server (blocking)
     * Starts all service rings and runs accept loop on connection ring
     */
    void run();

    /**
     * Stop the server
     * Stops accepting new connections and shuts down all service rings
     */
    void stop();

    /**
     * Get the router (for adding routes)
     */
    HttpRouter& getRouter() { return router_; }

    /**
     * Get the active configuration.
     */
    const ServerConfig& getConfig() const { return config_; }

    /**
     * Get number of service rings
     */
    int getNumRings() const { return config_.num_rings; }

    /**
     * Get current lifecycle state
     */
    ServerState getState() const { return state_.load(std::memory_order_acquire); }

    bool isRunning() const { return getState() == ServerState::Running; }
    bool isStopping() const { return getState() == ServerState::Stopping; }
    bool isAborting() const { return getState() == ServerState::Aborting; }
    bool isStopped() const { return getState() == ServerState::Stopped; }

private:
    int createServerSocket(int port, const std::string& bind_addr);

    // Join every started ring thread. Idempotent (ServiceRing::join() checks joinable()),
    // so run() and ~HttpServer() can both call it.
    void joinAllRings();

    ServerConfig config_;
    
    // Declaration order matters: members are destroyed in reverse, so service_rings_
    // must be declared *after* http_servers_. ~ServiceRing() joins its thread, and
    // that thread's completion handlers hold raw pointers into the SingleRingHttpServer
    // objects. Destroying http_servers_ while a ring thread is still draining is
    // a use-after-free (review item L11). ~HttpServer() also joins explicitly so this
    // does not rest on declaration order alone.
    std::vector<std::unique_ptr<SingleRingHttpServer>> http_servers_;    // One per service ring (internal)
    std::vector<std::unique_ptr<ServiceRing>> service_rings_;  // One per core
    
    HttpRouter router_;  // Shared router (read-only after setup)
    
    SSL_CTX* ssl_ctx_;
    std::atomic<ServerState> state_;
};

} // namespace caduvelox
