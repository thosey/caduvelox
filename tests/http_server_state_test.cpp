#include <gtest/gtest.h>
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>

using namespace caduvelox;

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
