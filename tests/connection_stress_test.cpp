#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

using namespace caduvelox;

// Use a fixed test port like other tests
constexpr int test_port = 18889;

class ConnectionStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up logging
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        
        // Initialize job server
        ASSERT_TRUE(job_server_.init(128));
        
        // Create HTTP server (without worker pool for simplicity)
        http_server_ = std::make_unique<HttpServer>(job_server_);
        
        // Add a simple test route
        http_server_->addRoute("GET", "/", [](const HttpRequest& req, HttpResponse& res) {
            res.setStatus(200);
            res.setBody("OK");
            res.setHeader("Content-Type", "text/plain");
        });
        
        // Listen on test port
        ASSERT_TRUE(http_server_->listen(test_port, "127.0.0.1"));
        
        // Start event loop
        event_loop_thread_ = std::thread([this]() {
            job_server_.run();
        });
        
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
    
    int connectToServer() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(test_port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return -1;
        }
        
        return sock;
    }
    
    Server job_server_;
    std::unique_ptr<HttpServer> http_server_;
    std::thread event_loop_thread_;
};

// Test: Connect and immediately disconnect without sending data
TEST_F(ConnectionStressTest, ConnectAndDisconnect) {
    for (int i = 0; i < 20; i++) {
        int sock = connectToServer();
        ASSERT_GT(sock, 0);
        close(sock);  // Immediate disconnect
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Give server time to process disconnections
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Server should still be alive and able to handle new connections
    int sock = connectToServer();
    ASSERT_GT(sock, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(write(sock, request, strlen(request)), 0);
    
    char buffer[1024];
    ssize_t n = read(sock, buffer, sizeof(buffer));
    ASSERT_GT(n, 0);
    close(sock);
}

// Test: Connect, send partial data, then disconnect
TEST_F(ConnectionStressTest, PartialRequestThenDisconnect) {
    for (int i = 0; i < 20; i++) {
        int sock = connectToServer();
        ASSERT_GT(sock, 0);
        
        // Send incomplete HTTP request
        const char* partial = "GET / HTTP";
        write(sock, partial, strlen(partial));
        
        // Immediately disconnect without completing request
        close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verify server still works
    int sock = connectToServer();
    ASSERT_GT(sock, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(write(sock, request, strlen(request)), 0);
    
    char buffer[1024];
    ssize_t n = read(sock, buffer, sizeof(buffer));
    ASSERT_GT(n, 0);
    close(sock);
}

// Test: Send garbage data
TEST_F(ConnectionStressTest, GarbageData) {
    for (int i = 0; i < 10; i++) {
        int sock = connectToServer();
        ASSERT_GT(sock, 0);
        
        // Send random garbage
        const char* garbage = "\x00\x01\x02\xFF\xFE\xFD random garbage data \r\n\r\n";
        write(sock, garbage, strlen(garbage));
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        close(sock);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verify server still works
    int sock = connectToServer();
    ASSERT_GT(sock, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(write(sock, request, strlen(request)), 0);
    
    char buffer[1024];
    ssize_t n = read(sock, buffer, sizeof(buffer));
    ASSERT_GT(n, 0);
    close(sock);
}

// Test: Rapid connect/disconnect cycles
TEST_F(ConnectionStressTest, RapidConnectDisconnect) {
    for (int i = 0; i < 50; i++) {
        int sock = connectToServer();
        ASSERT_GT(sock, 0);
        close(sock);
        // No delay - rapid fire
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    // Verify server still works
    int sock = connectToServer();
    ASSERT_GT(sock, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(write(sock, request, strlen(request)), 0);
    
    char buffer[1024];
    ssize_t n = read(sock, buffer, sizeof(buffer));
    ASSERT_GT(n, 0);
    close(sock);
}

// Test: Multiple concurrent connections that disconnect at different times
TEST_F(ConnectionStressTest, ConcurrentDisconnects) {
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([this, i]() {
            int sock = connectToServer();
            if (sock > 0) {
                // Some send data, some don't
                if (i % 2 == 0) {
                    const char* partial = "GET /";
                    write(sock, partial, strlen(partial));
                }
                
                // Disconnect at different times
                std::this_thread::sleep_for(std::chrono::milliseconds(i * 10));
                close(sock);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verify server still works
    int sock = connectToServer();
    ASSERT_GT(sock, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(write(sock, request, strlen(request)), 0);
    
    char buffer[1024];
    ssize_t n = read(sock, buffer, sizeof(buffer));
    ASSERT_GT(n, 0);
    close(sock);
}

// Test: Connect, wait for server to start reading, then disconnect during recv
TEST_F(ConnectionStressTest, DisconnectDuringRead) {
    for (int i = 0; i < 20; i++) {
        int sock = connectToServer();
        ASSERT_GT(sock, 0);
        
        // Give server time to register multishot recv
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        
        // Now disconnect while server is actively waiting for data
        close(sock);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verify server still works
    int sock = connectToServer();
    ASSERT_GT(sock, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(write(sock, request, strlen(request)), 0);
    
    char buffer[1024];
    ssize_t n = read(sock, buffer, sizeof(buffer));
    ASSERT_GT(n, 0);
    close(sock);
}

// Test: Send valid request, read response, then immediately close without proper shutdown
TEST_F(ConnectionStressTest, AbruptCloseAfterResponse) {
    for (int i = 0; i < 20; i++) {
        int sock = connectToServer();
        ASSERT_GT(sock, 0);
        
        const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        write(sock, request, strlen(request));
        
        // Start reading response
        char buffer[128];
        read(sock, buffer, sizeof(buffer));
        
        // Immediately close without reading full response or proper shutdown
        close(sock);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verify server still works
    int sock = connectToServer();
    ASSERT_GT(sock, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(write(sock, request, strlen(request)), 0);
    
    char buffer[1024];
    ssize_t n = read(sock, buffer, sizeof(buffer));
    ASSERT_GT(n, 0);
    close(sock);
}

// Test: Port scanner behavior - connect, check if open, disconnect
TEST_F(ConnectionStressTest, PortScannerBehavior) {
    for (int i = 0; i < 30; i++) {
        int sock = connectToServer();
        ASSERT_GT(sock, 0);
        
        // Port scanner often just checks if port is open then closes
        // Sometimes with RST flag (abrupt close)
        struct linger linger_opt = {1, 0};  // SO_LINGER with timeout 0 = send RST
        setsockopt(sock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt));
        
        close(sock);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verify server still works
    int sock = connectToServer();
    ASSERT_GT(sock, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(write(sock, request, strlen(request)), 0);
    
    char buffer[1024];
    ssize_t n = read(sock, buffer, sizeof(buffer));
    ASSERT_GT(n, 0);
    close(sock);
}
