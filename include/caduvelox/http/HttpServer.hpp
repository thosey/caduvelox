#pragma once

#include "caduvelox/ServiceRing.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/http/HttpRouter.hpp"
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
     * Create HTTPS server with optimized threading
     * @param num_rings Number of service rings (0 = auto-detect CPU cores)
     * @param queue_depth io_uring queue depth for each ring
     */
    explicit HttpServer(int num_rings = 0, unsigned queue_depth = 4096);
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
     * Get number of service rings
     */
    int getNumRings() const { return num_rings_; }

private:
    int createServerSocket(int port, const std::string& bind_addr);

    int num_rings_;
    unsigned queue_depth_;
    
    std::vector<std::unique_ptr<ServiceRing>> service_rings_;  // One per core
    std::vector<std::unique_ptr<SingleRingHttpServer>> http_servers_;    // One per service ring (internal)
    
    HttpRouter router_;  // Shared router (read-only after setup)
    
    SSL_CTX* ssl_ctx_;
    bool running_;
};

} // namespace caduvelox
