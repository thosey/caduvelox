#include <gtest/gtest.h>
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/jobs/IdleTimeoutJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

using namespace caduvelox;

/**
 * Item #17: keep-alive connections that go idle (no data, no close) between
 * requests must be closed by a server-side timer instead of holding a pool
 * slot and a recv job forever.
 */
class IdleTimeoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        ASSERT_TRUE(job_server_.init(256));

        http_server_ = std::make_unique<SingleRingHttpServer>(job_server_);
        http_server_->setIdleTimeoutMs(150);  // Short timeout for fast tests.

        http_server_->addRoute("GET", "/test", [](const HttpRequest&, HttpResponse& res) {
            res.setStatus(200);
            res.setBody("OK");
            res.setHeader("Content-Type", "text/plain");
            // No "Connection: close" — exercise the keep-alive idle-wait path.
        });

        event_loop_thread_ = std::thread([this]() { job_server_.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ASSERT_TRUE(http_server_->listen(test_port_, "127.0.0.1"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (http_server_) {
            http_server_->stop();
        }
        job_server_.stop();
        if (event_loop_thread_.joinable()) {
            event_loop_thread_.join();
        }
    }

    int createAndConnectSocket() {
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd < 0) {
            return -1;
        }
        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(test_port_);
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(client_fd);
            return -1;
        }
        return client_fd;
    }

    // Waits up to timeout_ms for the peer to close (POLLIN + recv()==0), or
    // for an error. Returns true if the connection was closed by the server.
    bool waitForPeerClose(int fd, int timeout_ms) {
        struct pollfd pfd{fd, POLLIN, 0};
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) {
            return false; // Timed out — server never closed.
        }
        char buf[16];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        return n == 0;
    }

    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_server_;
    std::thread event_loop_thread_;
    const int test_port_ = 8895;
};

TEST_F(IdleTimeoutTest, IdleKeepAliveConnectionIsClosedAfterTimeout) {
    int fd = createAndConnectSocket();
    ASSERT_GE(fd, 0);

    const char* request = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(send(fd, request, strlen(request), 0), 0);

    char buf[512];
    ASSERT_GT(recv(fd, buf, sizeof(buf), 0), 0) << "Expected a response before going idle";

    // Connection is now idle (keep-alive, no further requests sent). The
    // 150ms idle timeout should close it server-side well within 2 seconds.
    EXPECT_TRUE(waitForPeerClose(fd, 2000))
        << "Server did not close an idle keep-alive connection within the idle timeout";

    close(fd);
}

TEST_F(IdleTimeoutTest, ActivityBeforeTimeoutKeepsConnectionAlive) {
    int fd = createAndConnectSocket();
    ASSERT_GE(fd, 0);

    const char* request = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // Send three requests, each well inside the 150ms idle window, on the
    // same connection. None of them should trigger the idle-timeout close.
    for (int i = 0; i < 3; ++i) {
        ASSERT_GT(send(fd, request, strlen(request), 0), 0);
        char buf[512];
        ASSERT_GT(recv(fd, buf, sizeof(buf), 0), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // The connection should still be alive (no close pending) immediately
    // after the last response — only a brief poll, expecting no data/EOF yet.
    struct pollfd pfd{fd, POLLIN, 0};
    int ret = poll(&pfd, 1, 10);
    EXPECT_EQ(ret, 0) << "Connection was closed even though activity reset the idle timer";

    close(fd);
}

TEST_F(IdleTimeoutTest, IdleTimeoutJobPoolDoesNotLeak) {
    size_t initial_available = PoolManager::available<IdleTimeoutJob>();

    const int num_connections = 20;
    for (int i = 0; i < num_connections; ++i) {
        int fd = createAndConnectSocket();
        ASSERT_GE(fd, 0);
        const char* request = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ASSERT_GT(send(fd, request, strlen(request), 0), 0);
        char buf[512];
        ASSERT_GT(recv(fd, buf, sizeof(buf), 0), 0);

        EXPECT_TRUE(waitForPeerClose(fd, 2000))
            << "Connection " << i << " was not closed by the idle timeout";
        close(fd);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    size_t final_available = PoolManager::available<IdleTimeoutJob>();
    size_t leaked = initial_available - final_available;
    EXPECT_LE(leaked, 2) << "IdleTimeoutJob pool entries leaked across idle-timeout closes";
}
