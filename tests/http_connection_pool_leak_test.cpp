#include <gtest/gtest.h>
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>

using namespace caduvelox;

/**
 * Test for Issue #1: HttpConnectionJob Pool Leak
 * 
 * Verifies that HttpConnectionJob objects are properly returned to the pool
 * after connections are closed. Currently, they are allocated but never
 * deallocated, eventually exhausting the pool.
 * 
 * Expected Result: This test should FAIL before the fix, showing that pool
 * entries are permanently consumed by completed connections.
 */
class HttpConnectionPoolLeakTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        
        ASSERT_TRUE(job_server_.init(256));
        
        http_server_ = std::make_unique<SingleRingHttpServer>(job_server_);
        
        // Add a simple route
        http_server_->addRoute("GET", "/test", [](const HttpRequest& req, HttpResponse& res) {
            res.setStatus(200);
            res.setBody("OK");
            res.setHeader("Content-Type", "text/plain");
            res.setHeader("Connection", "close");  // Force connection close
        });
        
        // Start event loop
        event_loop_thread_ = std::thread([this]() {
            job_server_.run();
        });
        
        // Give event loop time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Start listening
        ASSERT_TRUE(http_server_->listen(test_port_, "127.0.0.1"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    void TearDown() override {
        if (http_server_) {
            http_server_->stop();
        }
        
        stop_requested_ = true;
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
        
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(test_port_);
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(client_fd);
            return -1;
        }
        
        return client_fd;
    }
    
    bool sendRequestAndClose(int client_fd) {
        const char* request = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ssize_t sent = send(client_fd, request, strlen(request), 0);
        if (sent <= 0) {
            return false;
        }
        
        // Read response (brief)
        char buffer[512];
        recv(client_fd, buffer, sizeof(buffer), 0);
        
        close(client_fd);
        return true;
    }
    
    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_server_;
    std::thread event_loop_thread_;
    std::atomic<bool> stop_requested_{false};
    const int test_port_ = 8890;
};

TEST_F(HttpConnectionPoolLeakTest, ShortLivedConnectionsLeakPool) {
    // Get initial pool state
    size_t initial_available = PoolManager::available<HttpConnectionJob>();
    size_t initial_allocated = PoolManager::allocated<HttpConnectionJob>();
    
    std::cout << "Initial HttpConnectionJob pool - available: " << initial_available
              << ", allocated: " << initial_allocated << std::endl;
    
    const int num_connections = 100;
    int successful_connections = 0;
    
    std::cout << "Opening " << num_connections << " short-lived connections..." << std::endl;
    
    for (int i = 0; i < num_connections; i++) {
        int client_fd = createAndConnectSocket();
        if (client_fd >= 0) {
            if (sendRequestAndClose(client_fd)) {
                successful_connections++;
            }
        }
        
        // Small delay between connections
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "Successfully completed " << successful_connections << " connections" << std::endl;
    
    // Wait for all connections to fully close
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Check pool state after connections
    size_t final_available = PoolManager::available<HttpConnectionJob>();
    size_t final_allocated = PoolManager::allocated<HttpConnectionJob>();
    
    std::cout << "Final HttpConnectionJob pool - available: " << final_available
              << ", allocated: " << final_allocated << std::endl;
    
    size_t leaked_entries = initial_available - final_available;
    std::cout << "Pool entries leaked: " << leaked_entries << std::endl;
    
    // The pool should have recovered most entries
    // This assertion should FAIL before the fix
    EXPECT_LE(leaked_entries, 10)
        << "HttpConnectionJob pool entries not returned after connections closed. "
        << "Leaked " << leaked_entries << " entries from " << successful_connections << " connections.";
    
    // Check allocated count
    size_t extra_allocated = final_allocated - initial_allocated;
    EXPECT_LE(extra_allocated, 10)
        << "Too many HttpConnectionJob entries remain allocated: " << extra_allocated;
}

TEST_F(HttpConnectionPoolLeakTest, PoolExhaustionAfterManyConnections) {
    // The pool has 10,000 entries by default
    // This test verifies we can handle many connections without exhaustion
    
    size_t initial_available = PoolManager::available<HttpConnectionJob>();
    std::cout << "Initial available entries: " << initial_available << std::endl;
    
    const int batch_size = 50;
    const int num_batches = 20;  // Total 1000 connections
    
    for (int batch = 0; batch < num_batches; batch++) {
        std::cout << "Batch " << batch << "..." << std::endl;
        
        for (int i = 0; i < batch_size; i++) {
            int client_fd = createAndConnectSocket();
            if (client_fd >= 0) {
                sendRequestAndClose(client_fd);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        
        // Check pool between batches
        size_t current_available = PoolManager::available<HttpConnectionJob>();
        std::cout << "  After batch " << batch << ", available: " << current_available 
                  << " (leaked: " << (initial_available - current_available) << ")" << std::endl;
        
        // Wait for cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    size_t final_available = PoolManager::available<HttpConnectionJob>();
    size_t total_leaked = initial_available - final_available;
    
    std::cout << "Total leaked after " << (batch_size * num_batches) << " connections: " 
              << total_leaked << std::endl;
    
    // With the bug, we'd leak 1000 entries
    // This should FAIL before the fix
    EXPECT_LE(total_leaked, 50)
        << "Massive pool leak detected across many connections.";
}

TEST_F(HttpConnectionPoolLeakTest, PoolReusableAcrossCycles) {
    // Verify pool entries can be reused, not just leaked indefinitely
    
    auto runCycle = [this](const std::string& label) {
        size_t start = PoolManager::available<HttpConnectionJob>();
        
        for (int i = 0; i < 30; i++) {
            int fd = createAndConnectSocket();
            if (fd >= 0) {
                sendRequestAndClose(fd);
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        size_t end = PoolManager::available<HttpConnectionJob>();
        size_t leaked = start - end;
        
        std::cout << label << " - leaked: " << leaked << std::endl;
        return leaked;
    };
    
    size_t leaked1 = runCycle("Cycle 1");
    size_t leaked2 = runCycle("Cycle 2");
    size_t leaked3 = runCycle("Cycle 3");
    
    // Each cycle should leak approximately the same amount (if entries are being reused)
    // Before the fix, we'd see: leaked1=30, leaked2=30, leaked3=30
    // After the fix: leaked1≈0, leaked2≈0, leaked3≈0
    
    EXPECT_LE(leaked1, 5) << "Cycle 1 leaked pool entries";
    EXPECT_LE(leaked2, 5) << "Cycle 2 leaked pool entries";
    EXPECT_LE(leaked3, 5) << "Cycle 3 leaked pool entries";
}
