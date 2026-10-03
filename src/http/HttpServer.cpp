#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/http/PortRange.hpp"
#include "caduvelox/jobs/KTLSContextHelper.hpp"
#include "caduvelox/jobs/KTLSJob.hpp"
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/jobs/IdleTimeoutJob.hpp"
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace caduvelox {

HttpServer::HttpServer(const ServerConfig& cfg)
    : config_(cfg),
      ssl_ctx_(nullptr),
      state_(ServerState::Stopped) {

    // Resolve num_rings=0 (auto-detect) before anything else.
    if (config_.num_rings <= 0) {
        config_.num_rings = static_cast<int>(std::thread::hardware_concurrency());
    }

    // Validate config before touching any pools — throws std::invalid_argument
    // on zero pool sizes so misconfiguration is caught at construction time.
    config_.validate();

    // Apply pool capacities from config before any ring threads are started.
    // Thread-local pools are initialised on first access, so these writes are
    // visible to every ring thread created during listenKTLS().
    PoolManager::setCapacity<KTLSJob>(config_.ktls_pool_size);
    PoolManager::setCapacity<AcceptJob>(config_.accept_pool_size);
    PoolManager::setCapacity<WriteJob>(config_.write_pool_size);
    PoolManager::setCapacity<HTTPFileJob>(config_.file_job_pool_size);
    PoolManager::setCapacity<HttpConnectionJob>(config_.connection_pool_size);
    PoolManager::setCapacity<HttpMultishotRecvJob>(config_.connection_pool_size);
    PoolManager::setCapacity<IdleTimeoutJob>(config_.connection_pool_size);

    // Log startup resource footprint.
    const size_t buf_mb = (static_cast<size_t>(config_.buffer_ring_count) *
                           config_.buffer_size_bytes) / (1024 * 1024);
    Logger::info("HttpServer: Creating server with " +
                                     std::to_string(config_.num_rings) + " service rings");
    Logger::info("HttpServer: Buffer ring: " +
                                     std::to_string(config_.buffer_ring_count) + " x " +
                                     std::to_string(config_.buffer_size_bytes) + " B = " +
                                     std::to_string(buf_mb) + " MB per ring");
    Logger::info("HttpServer: kTLS pool: " +
                                     std::to_string(config_.ktls_pool_size) + " slots");
    Logger::info("HttpServer: Connection pool: " +
                                     std::to_string(config_.connection_pool_size) + " slots");
}

HttpServer::HttpServer(int num_rings, unsigned queue_depth)
    : HttpServer([&]{
        ServerConfig cfg;
        cfg.num_rings    = num_rings;
        cfg.queue_depth  = queue_depth;
        return cfg;
    }()) {
}

HttpServer::~HttpServer() {
    stop();

    // Join before anything is torn down. Two things depend on this, and neither is
    // covered by member destruction order (review item L11):
    //
    //   1. Ring threads run completion handlers that dereference the
    //      SingleRingHttpServer objects in http_servers_.
    //   2. ssl_ctx_ is freed by hand right below, and a ring thread mid-handshake
    //      is still using it.
    //
    // run() joins too, but only on the path where it was called and returned. A
    // HttpServer destroyed without a completed run() -- an exception unwinding past
    // it, a listen() that succeeded but was never run, a stop() from another thread
    // -- would otherwise free all of this out from under live ring threads.
    joinAllRings();

    if (ssl_ctx_) {
        KTLSContextHelper::freeContext(ssl_ctx_);
    }
}

void HttpServer::joinAllRings() {
    for (auto& ring : service_rings_) {
        if (ring) {
            ring->join();
        }
    }
}

// Routes added once the server is listening reach nobody: startRings() copies the
// router into each ring's SingleRingHttpServer as it starts it, and nothing reads
// this copy again. Say so rather than accepting a route that will never match --
// examples/rest_api_server was registering routes after listen() and losing them
// silently, which is what this guard is for.
//
// Sharing the router with the rings instead would mean mutating routing state
// while ring threads dispatch against it; the per-ring copy exists so dispatch
// touches read-only data. Refusing is the cheaper correctness.
bool HttpServer::routesStillAccepted(const char* what) const {
    if (isStopped()) {
        return true;
    }
    Logger::getInstance().logError(
        std::string("HttpServer: ") + what + " called after the server started listening; "
        "the route would never match. Register every route before listen()/listenKTLS().");
    return false;
}

void HttpServer::addRoute(const std::string& method, const std::string& path_pattern,
                                    HttpHandler handler) {
    if (!routesStillAccepted("addRoute()")) {
        return;
    }
    router_.addRoute(method, path_pattern, std::move(handler));
}

void HttpServer::addRouteWithCaptures(const std::string& method, const std::string& path_pattern,
                                      HttpHandlerWithCaptures handler) {
    if (!routesStillAccepted("addRouteWithCaptures()")) {
        return;
    }
    router_.addRouteWithCaptures(method, path_pattern, std::move(handler));
}

bool HttpServer::canStartListening(const char* what) const {
    if (!isStopped()) {
        Logger::getInstance().logError(
            std::string("HttpServer: ") + what + " called while the server is not stopped");
        return false;
    }

    // A server that has already listened cannot listen again, and the state check
    // above cannot catch it: a fresh HttpServer is constructed Stopped, and stop()
    // returns it to Stopped, so isStopped() permits the first call and a second one
    // after a restart alike.
    //
    // Nothing resets the ring state between those calls. startRings() only ever
    // reserves and push_back()s, so a second call appended a whole second set of
    // rings to the first set -- leaving twice the configured number, half of them
    // belonging to the previous incarnation with already-stopped, already-drained
    // Servers, and run() then starting threads on all of them. getNumRings() went
    // on reporting config_.num_rings, which no longer matched service_rings_.size().
    // listenKTLS() also overwrote ssl_ctx_ without freeing the first one, so a
    // restart dropped an OpenSSL context on the floor.
    //
    // Refused rather than supported. Making a restart work means destroying the
    // previous rings, and their threads have to be joined before the
    // SingleRingHttpServer objects their completion handlers dereference are
    // destroyed -- the opposite of the declaration order that keeps the normal
    // teardown correct (see ~HttpServer()). That ordering is where review item L11
    // lived, so it is not a change to make on the way past. Construct a new
    // HttpServer instead; it costs one object.
    if (!service_rings_.empty()) {
        Logger::getInstance().logError(
            std::string("HttpServer: ") + what + " called on a server that has already "
            "listened; a restart is not supported. Construct a new HttpServer instead.");
        return false;
    }

    return true;
}

bool HttpServer::listen(int port, const std::string& bind_addr) {
    if (!canStartListening("listen()")) {
        return false;
    }

    // Plain HTTP. This did not exist: the only public entry point was
    // listenKTLS(), so a multi-ring server could not serve HTTP at all -- which is
    // what a service behind a TLS-terminating proxy wants, and what
    // examples/rest_api_server had been written against.
    return startRings(port, bind_addr, /*use_ktls=*/false);
}

bool HttpServer::listenKTLS(int port, const std::string& cert_path, 
                                      const std::string& key_path,
                                      const std::string& bind_addr) {
    // Checked before the context is created, not after: assigning ssl_ctx_ is what
    // used to leak the previous one, so a refusal has to happen first.
    if (!canStartListening("listenKTLS()")) {
        return false;
    }

    // Create SSL context for KTLS
    ssl_ctx_ = KTLSContextHelper::createServerContext(cert_path, key_path);
    if (!ssl_ctx_) {
        Logger::getInstance().logError("HttpServer: Failed to create SSL context");
        return false;
    }

    return startRings(port, bind_addr, /*use_ktls=*/true);
}

// One listening socket per ring, all on the same port via SO_REUSEPORT, each with
// its own SingleRingHttpServer sharing the router.
//
// Shared by listen() and listenKTLS(), which differ only in whether an accepted
// connection starts a TLS handshake and whether an SSL context exists to hand
// down. Copying this loop for the plain case would have been a second place for
// ring setup to drift.
bool HttpServer::startRings(int port, const std::string& bind_addr, bool use_ktls) {
    service_rings_.reserve(config_.num_rings);
    http_servers_.reserve(config_.num_rings);

    for (int i = 0; i < config_.num_rings; ++i) {
        // Create service ring with CPU affinity and runtime buffer config
        int cpu_id = i % static_cast<int>(std::thread::hardware_concurrency());
        auto ring = std::make_unique<ServiceRing>(i, cpu_id, config_.queue_depth,
                                                  config_.buffer_ring_count,
                                                  config_.buffer_size_bytes);

        if (!ring->init()) {
            Logger::getInstance().logError("HttpServer: Failed to initialize service ring " + 
                                          std::to_string(i));
            return false;
        }

        // Create listening socket with SO_REUSEPORT for this ring
        int server_fd = createServerSocket(port, bind_addr);
        if (server_fd < 0) {
            Logger::getInstance().logError("HttpServer: Failed to create socket for ring " + 
                                          std::to_string(i));
            return false;
        }

        // Create internal SingleRingHttpServer for this ring
        // HTTP processing happens inline on io_uring thread for maximum performance
        auto http_server = std::make_unique<SingleRingHttpServer>(ring->getServer());

        // Share the router (read-only after setup)
        http_server->setRouter(router_);

        // Set KTLS context (borrowed reference - parent HttpServer owns it).
        // Null for a plain listener, which never starts a handshake.
        if (use_ktls) {
            http_server->setKTLSContext(ssl_ctx_, false);
        }

        // Apply per-ring runtime config
        http_server->setKtlsHandshakeTimeoutMs(config_.ktls_handshake_timeout_ms);
        http_server->setIdleTimeoutMs(config_.idle_timeout_ms);

        // Start listening on this ring's socket
        // We manually set the socket since we created it with SO_REUSEPORT
        // HttpServer will do the accept multishot on this fd
        if (!http_server->listenOnFd(server_fd, use_ktls)) {
            Logger::getInstance().logError("HttpServer: Failed to start listening on ring " + 
                                          std::to_string(i));
            close(server_fd);
            return false;
        }

        // Share the canonical server state so ring-local jobs can observe it.
        ring->bindToServerState(&state_);

        service_rings_.push_back(std::move(ring));
        http_servers_.push_back(std::move(http_server));

        Logger::info("HttpServer: Ring " + std::to_string(i) + 
                                        " initialized and listening");
    }

    state_.store(ServerState::Running, std::memory_order_release);

    Logger::info("HttpServer: All " + std::to_string(config_.num_rings) + " rings listening on " +
                 bind_addr + ":" + std::to_string(port) + (use_ktls ? " (HTTPS)" : " (HTTP)"));

    return true;
}

void HttpServer::run() {
    if (!isRunning()) {
        Logger::getInstance().logError("HttpServer: Server not initialized");
        return;
    }

    // Start all service ring threads - each runs its own accept + processing loop
    for (auto& ring : service_rings_) {
        ring->start();
    }
    
    Logger::info("HttpServer: All service rings started");

    // Wait for all service rings to finish
    joinAllRings();

    state_.store(ServerState::Stopped, std::memory_order_release);
    
    Logger::info("HttpServer: All service rings stopped");
}

void HttpServer::stop() {
    // The CAS decides who *announces* the transition, not whether the shutdown work
    // runs. It cannot gate the work, because state_ is shared: every ring's Server is
    // bound to this same atomic (see the bindToServerState call in listen()), so any
    // single ring calling Server::stop() already flips it to Stopping. A later
    // HttpServer::stop() -- including the one in ~HttpServer() -- would then find the
    // CAS failing and return without stopping the *other* rings or closing their
    // listening sockets, leaving threads running into destruction (review item L11).
    //
    // Every call below is idempotent (SingleRingHttpServer::stop() and
    // ServiceRing::stop() both guard on their own running_ flag), so running them on
    // a second call is free.
    ServerState expected = ServerState::Running;
    const bool announced = state_.compare_exchange_strong(expected, ServerState::Stopping,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire);

    if (state_.load(std::memory_order_acquire) == ServerState::Stopped &&
        service_rings_.empty()) {
        return;  // never started; nothing to wind down
    }

    // Stop all HttpServers (which stops accepting)
    for (auto& http_server : http_servers_) {
        if (http_server) {
            http_server->stop();
        }
    }

    // Stop all service rings
    for (auto& ring : service_rings_) {
        if (ring) {
            ring->stop();
        }
    }

    // If run() has not started the ring threads yet, or if they have already
    // fully unwound, there is no join path left to transition us to Stopped.
    bool any_ring_running = false;
    for (const auto& ring : service_rings_) {
        if (ring && ring->isRunning()) {
            any_ring_running = true;
            break;
        }
    }

    if (!any_ring_running) {
        state_.store(ServerState::Stopped, std::memory_order_release);
    }

    if (announced) {
        Logger::info("HttpServer: Server stopped");
    }
}

int HttpServer::createServerSocket(int port, const std::string& bind_addr) {
    // The multi-ring path never checked this, and sin_port is 16 bits: htons()
    // truncated anything out of range and the server bound a port nobody asked
    // for. Now that listen() is public as well as listenKTLS(), that is reachable
    // from the ordinary entry point.
    if (!port::isValid(port)) {
        Logger::getInstance().logError("HttpServer: " + port::rejection(port));
        return -1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        Logger::getInstance().logError("HttpServer: Failed to create socket: " + 
                                      std::string(strerror(errno)));
        return -1;
    }

    // Set SO_REUSEADDR and SO_REUSEPORT - critical for multi-ring!
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to set SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to set SO_REUSEPORT");
        close(server_fd);
        return -1;
    }

    // Bind to address
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int inet_result = inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr);
    if (inet_result == 0) {
        Logger::getInstance().logError("HttpServer: Invalid IPv4 address format: " + bind_addr);
        close(server_fd);
        return -1;
    } else if (inet_result < 0) {
        Logger::getInstance().logError("HttpServer: inet_pton failed for address " + bind_addr +
                                       ": " + std::string(strerror(errno)));
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to bind to port " + 
                                      std::to_string(port) + ": " + std::string(strerror(errno)));
        close(server_fd);
        return -1;
    }

    // Start listening. Qualified: HttpServer::listen() would otherwise shadow the
    // POSIX listen(2) being called here.
    if (::listen(server_fd, SOMAXCONN) < 0) {
        Logger::getInstance().logError("HttpServer: Failed to listen: " + 
                                      std::string(strerror(errno)));
        close(server_fd);
        return -1;
    }

    return server_fd;
}

} // namespace caduvelox
