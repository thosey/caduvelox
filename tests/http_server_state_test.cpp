#include <gtest/gtest.h>
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <chrono>

using namespace caduvelox;

namespace {

// True if a plain socket can bind `port`, i.e. nothing still holds it.
//
// Deliberately does NOT set SO_REUSEPORT. Every ring's listening socket does set
// it, and SO_REUSEPORT only lets sockets share a port when *all* of them opt in --
// so while any ring listener is still open this bind fails with EADDRINUSE. That
// is what makes it a usable probe for "were the listeners actually closed?".
bool portIsFree(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    close(fd);
    return bound;
}

// portIsFree() with a bounded wait, because the release is not synchronous with
// close(2) and this is not flake padding.
//
// startAccepting() submits a multishot IORING_OP_ACCEPT on each listening socket.
// A submitted operation holds a kernel reference to the underlying struct file, so
// closing the descriptor does not free the socket while that accept is still parked
// -- and when run() was never called, nothing ever reaped it. The socket is released
// when the ring itself is torn down by io_uring_queue_exit() in ~Server(), and that
// teardown finishes asynchronously. Measured at one 20 ms tick on 6.x/7.x; the
// generous bound is for loaded CI, not for hiding a leak. A genuine leak never
// resolves and still fails.
bool waitForPortFree(int port, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (portIsFree(port)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return portIsFree(port);
}

} // namespace

class HttpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
    }

    void addBasicRoute(HttpServer& server) {
        server.addRoute("GET", "/", [](const HttpRequest&, HttpResponse& res) {
            res.setStatus(200);
            res.setBody("OK");
        });
    }

    static constexpr const char* kCertPath = "test_cert.pem";
    static constexpr const char* kKeyPath = "test_key.pem";
};

TEST_F(HttpServerTest, InitialStateIsStopped) {
    HttpServer server(1, 128);
    EXPECT_EQ(server.getState(), ServerState::Stopped);
    EXPECT_TRUE(server.isStopped());
    EXPECT_FALSE(server.isRunning());
    EXPECT_FALSE(server.isStopping());
    EXPECT_FALSE(server.isAborting());
}

TEST_F(HttpServerTest, StopWithoutStartLeavesStateStopped) {
    HttpServer server(1, 128);
    server.stop();

    EXPECT_EQ(server.getState(), ServerState::Stopped);
    EXPECT_TRUE(server.isStopped());
}

TEST_F(HttpServerTest, ListenTransitionsStateToRunning) {
    const int test_port = 8460;

    HttpServer server(2, 128);
    addBasicRoute(server);

    ASSERT_EQ(server.getState(), ServerState::Stopped);

    if (!server.listenKTLS(test_port, kCertPath, kKeyPath)) {
        GTEST_SKIP() << "KTLS not available, skipping state transition test";
    }

    EXPECT_EQ(server.getState(), ServerState::Running);
    EXPECT_TRUE(server.isRunning());
    EXPECT_FALSE(server.isStopped());

    // stop() before run() should return directly to Stopped
    server.stop();

    EXPECT_EQ(server.getState(), ServerState::Stopped);
    EXPECT_TRUE(server.isStopped());
}

TEST_F(HttpServerTest, RunAndStopEndsInStoppedState) {
    const int test_port = 8461;

    HttpServer server(2, 128);
    addBasicRoute(server);

    if (!server.listenKTLS(test_port, kCertPath, kKeyPath)) {
        GTEST_SKIP() << "KTLS not available, skipping run/stop state test";
    }

    ASSERT_EQ(server.getState(), ServerState::Running);

    std::thread run_thread([&server]() {
        server.run();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.getState(), ServerState::Running);

    server.stop();

    run_thread.join();

    EXPECT_EQ(server.getState(), ServerState::Stopped);
    EXPECT_TRUE(server.isStopped());
    EXPECT_FALSE(server.isRunning());
    EXPECT_FALSE(server.isStopping());
}

// Verify that stop() transitions through Stopping before settling on Stopped.
// The transition is captured by observing the state on the calling thread
// immediately after stop() begins (before run_thread unwinds).
TEST_F(HttpServerTest, StopTransitionsThroughStopping) {
    const int test_port = 8462;

    HttpServer server(1, 128);
    addBasicRoute(server);

    if (!server.listenKTLS(test_port, kCertPath, kKeyPath)) {
        GTEST_SKIP() << "KTLS not available, skipping stopping-state transition test";
    }

    ASSERT_EQ(server.getState(), ServerState::Running);

    std::atomic<ServerState> state_during_stop{ServerState::Running};

    std::thread run_thread([&server]() {
        server.run();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Capture state on the calling thread as soon as stop() sets it.
    // stop() uses compare_exchange_strong Running→Stopping before waking rings,
    // so on this thread we should see Stopping immediately after the call returns.
    std::thread stop_thread([&]() {
        server.stop();
        state_during_stop.store(server.getState(), std::memory_order_relaxed);
    });

    stop_thread.join();
    run_thread.join();

    // After everything finishes the state must be Stopped.
    EXPECT_EQ(server.getState(), ServerState::Stopped);

    // The captured state should be either Stopping (rings still alive) or Stopped
    // (rings already drained by the time stop() returns). Both are valid.
    ServerState captured = state_during_stop.load();
    EXPECT_TRUE(captured == ServerState::Stopping || captured == ServerState::Stopped)
        << "Expected Stopping or Stopped, got: " << static_cast<int>(captured);
}

// Verify that double stop() is idempotent.
TEST_F(HttpServerTest, DoubleStopIsIdempotent) {
    const int test_port = 8463;

    HttpServer server(1, 128);
    addBasicRoute(server);

    if (!server.listenKTLS(test_port, kCertPath, kKeyPath)) {
        GTEST_SKIP() << "KTLS not available, skipping double-stop test";
    }

    std::thread run_thread([&server]() {
        server.run();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    server.stop();
    server.stop();  // Should be a no-op

    run_thread.join();

    EXPECT_EQ(server.getState(), ServerState::Stopped);
}

// --- Destruction releases listening sockets ---
//
// Read the scope of these honestly. They were written alongside the L11 fix
// (~HttpServer() now joins the ring threads before freeing ssl_ctx_ and
// http_servers_, and the member declaration order was corrected), but they do NOT
// cover it: they pass with and without that fix, because ~SingleRingHttpServer()
// closes its own listening fd during member destruction either way. Verified by
// removing the destructor's stop() call and re-running -- both still passed.
//
// L11's actual trigger is an exception unwinding out of run()'s ring-start loop,
// leaving started-but-unjoined threads. There is no way to produce that from the
// public API without the fault-injection seams review item L10 asks for, so no
// discriminating regression test exists for it yet.
//
// What these two do pin is worth having on its own: a destroyed HttpServer must not
// leave listening sockets behind. Writing them also turned up something worth
// knowing -- after a listen() that is never run(), the port stays bound for a moment
// past the destructor. Not a leak; see waitForPortFree() above for why the kernel
// releases it asynchronously.

TEST_F(HttpServerTest, DestructorReleasesListenersWhenRunWasNeverCalled) {
    const int test_port = 8464;

    ASSERT_TRUE(portIsFree(test_port)) << "test port already in use";

    {
        HttpServer server(2, 128);
        addBasicRoute(server);

        if (!server.listenKTLS(test_port, kCertPath, kKeyPath)) {
            GTEST_SKIP() << "KTLS not available, skipping destructor teardown test";
        }

        ASSERT_FALSE(portIsFree(test_port))
            << "sanity check failed: the running server should be holding the port";
    }

    EXPECT_TRUE(waitForPortFree(test_port))
        << "~HttpServer must close every ring's listening socket even though run() "
           "was never called";
}

TEST_F(HttpServerTest, DestructorReleasesListenersAfterRunAndStop) {
    const int test_port = 8465;

    ASSERT_TRUE(portIsFree(test_port)) << "test port already in use";

    {
        HttpServer server(2, 128);
        addBasicRoute(server);

        if (!server.listenKTLS(test_port, kCertPath, kKeyPath)) {
            GTEST_SKIP() << "KTLS not available, skipping destructor teardown test";
        }

        std::thread run_thread([&server]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        server.stop();
        run_thread.join();

        ASSERT_EQ(server.getState(), ServerState::Stopped);
    }

    EXPECT_TRUE(waitForPortFree(test_port))
        << "listening sockets outlived ~HttpServer";
}
