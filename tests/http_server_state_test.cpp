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
