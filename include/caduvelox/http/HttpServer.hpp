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
     * Add a route to all HttpServer instances.
     *
     * Must be called before listen()/listenKTLS(): each ring copies the router as
     * it starts, so a route added later would never match. Calling it late logs an
     * error and changes nothing.
     */
    void addRoute(const std::string& method, const std::string& path_pattern,
                  HttpHandler handler);

    /**
     * Add a route whose handler receives the regex captures.
     *
     * The router percent-decodes the path before matching, so a capture arrives
     * decoded and without the query string -- which is where a handler should
     * take a filename from. Without this, a multi-ring application had to
     * re-derive it from req.path, stripping the query and decoding by hand.
     *
     * Must be called before listenKTLS().
     */
    void addRouteWithCaptures(const std::string& method, const std::string& path_pattern,
                              HttpHandlerWithCaptures handler);

    /**
     * Start a plain HTTP server on the given port.
     *
     * One listening socket per ring, sharing the port via SO_REUSEPORT. Use this
     * behind a proxy that terminates TLS; use listenKTLS() to terminate it here.
     * @return true on success, false on failure
     */
    bool listen(int port, const std::string& bind_addr = "0.0.0.0");

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
    // Shared by listen() and listenKTLS(): one socket and one
    // SingleRingHttpServer per ring, differing only in whether TLS is terminated.
    // Refuse route registration once listening, with an explanation. See the .cpp.
    bool routesStillAccepted(const char* what) const;
    // Shared by listen() and listenKTLS(): refuse a call on a server that is not
    // stopped, and refuse a second one on a server that has already listened.
    // Called before listenKTLS() creates its SSL context, which a refusal would
    // otherwise leak. See the .cpp for why a restart is refused rather than made
    // to work.
    bool canStartListening(const char* what) const;
    bool startRings(int port, const std::string& bind_addr, bool use_ktls);
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
