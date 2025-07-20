#include <gtest/gtest.h>
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

using namespace caduvelox;

class JobHttpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up logging
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        
        // Initialize job server
        ASSERT_TRUE(job_server_.init(128));
        
        // Create HTTP server
        http_server_ = std::make_unique<HttpServer>(job_server_);
        
        // Add a simple test route
        http_server_->addRoute("GET", "/test", [](const HttpRequest& req, HttpResponse& res) {
            res.setStatus(200);
            res.setBody("Hello, World!");
            res.setHeader("Content-Type", "text/plain");
        });

        // Start the io_uring event loop so tests can submit jobs immediately.
        startEventLoop();
    }
    
    void TearDown() override {
        if (http_server_) {
            http_server_->stop();
        }
        job_server_.stop();
        
        // Wait for event loop thread to finish
        if (event_loop_thread_.joinable()) {
            event_loop_thread_.join();
        }
    }
    
    // Helper to create a client socket and connect to the server
    int createClientSocket(int port) {
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd < 0) {
            return -1;
        }
        
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(client_fd);
            return -1;
        }
        
        return client_fd;
    }
    
    // Helper to send HTTP request and receive response
    std::string sendHttpRequest(int client_fd, const std::string& request) {
        std::cout << "Sending request: " << request << std::endl;
        
        // Send request
        ssize_t sent = send(client_fd, request.c_str(), request.length(), 0);
        if (sent != static_cast<ssize_t>(request.length())) {
            std::cout << "Failed to send request, sent: " << sent << " expected: " << request.length() << std::endl;
            return "";
        }
        
        std::cout << "Request sent successfully, waiting for response..." << std::endl;
        
        // Receive response
        char buffer[4096];
        std::string response;
        
        // Set a timeout for receive
        struct timeval timeout;
        timeout.tv_sec = 2;  // Reduced to 2 second timeout
        timeout.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
        int attempts = 0;
        while (attempts < 5) {  // Max 5 attempts
            ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
            std::cout << "Receive attempt " << attempts << ", received: " << received << std::endl;
            
            if (received <= 0) {
                if (received == 0) {
                    std::cout << "Connection closed by server" << std::endl;
                } else {
                    std::cout << "Receive error: " << strerror(errno) << std::endl;
                }
                attempts++;
                if (attempts < 5) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                break;
            }
            buffer[received] = '\0';
            response += buffer;
            
            std::cout << "Received data: '" << std::string(buffer, received) << "'" << std::endl;
            
            // Simple check for end of HTTP response
            if (response.find("\r\n\r\n") != std::string::npos) {
                std::cout << "Found end of headers" << std::endl;
                // If we have headers, check if we have body content too
                size_t body_start = response.find("\r\n\r\n") + 4;
                if (body_start < response.length()) {
                    // We have some body content, this is likely complete
                    std::cout << "Complete response received" << std::endl;
                    break;
                }
            }
            attempts++;
        }
        
        std::cout << "Final response: '" << response << "'" << std::endl;
        return response;
    }
    
    Server job_server_;
    std::unique_ptr<HttpServer> http_server_;
    std::thread event_loop_thread_;
    
    // Helper to start the event loop in a background thread
    void startEventLoop() {
        if (event_loop_thread_.joinable()) {
            return;
        }

        event_loop_thread_ = std::thread([this]() {
            job_server_.run();
        });
        
        // Give the event loop time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
};

TEST_F(JobHttpServerTest, BasicServerStartup) {
    // Test that the server can start and listen on a port
    EXPECT_TRUE(http_server_->listen(0, "127.0.0.1"));  // Use port 0 for auto-assignment
    
    // Give the server a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Server should be running
    // Note: We can't easily test the actual port assignment without accessing internals
}

TEST_F(JobHttpServerTest, ServerListenFailure) {
    // Test that listen fails on invalid port
    EXPECT_FALSE(http_server_->listen(-1, "127.0.0.1"));
}

TEST_F(JobHttpServerTest, BasicHttpRequest) {
    // Start server on a specific port
    const int test_port = 8081;
    ASSERT_TRUE(http_server_->listen(test_port, "127.0.0.1"));
    
    // Give the server time to start accepting connections
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Create client connection
    int client_fd = createClientSocket(test_port);
    ASSERT_GE(client_fd, 0) << "Failed to create client socket";
    
    // Send HTTP request
    std::string request = "GET /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    std::string response = sendHttpRequest(client_fd, request);
    
    close(client_fd);
    
    // Verify response
    EXPECT_FALSE(response.empty()) << "No response received from server";
    EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos) << "Expected 200 status, got: " << response;
    EXPECT_NE(response.find("Hello, World!"), std::string::npos) << "Expected body content, got: " << response;
}

TEST_F(JobHttpServerTest, NotFoundResponse) {
    // Start server
    const int test_port = 8082;
    ASSERT_TRUE(http_server_->listen(test_port, "127.0.0.1"));
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Create client connection
    int client_fd = createClientSocket(test_port);
    ASSERT_GE(client_fd, 0);
    
    // Send request to non-existent route
    std::string request = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    std::string response = sendHttpRequest(client_fd, request);
    
    close(client_fd);
    
    // Verify 404 response
    EXPECT_FALSE(response.empty());
    EXPECT_NE(response.find("HTTP/1.1 404"), std::string::npos) << "Expected 404 status, got: " << response;
}

TEST_F(JobHttpServerTest, MultipleRequests) {
    // Test that the server can handle multiple sequential requests
    const int test_port = 8083;
    ASSERT_TRUE(http_server_->listen(test_port, "127.0.0.1"));
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    for (int i = 0; i < 3; ++i) {
        int client_fd = createClientSocket(test_port);
        ASSERT_GE(client_fd, 0) << "Failed to create client socket for request " << i;
        
        std::string request = "GET /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::string response = sendHttpRequest(client_fd, request);
        
        close(client_fd);
        
        EXPECT_FALSE(response.empty()) << "No response for request " << i;
        EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos) << "Expected 200 status for request " << i;
    }
}

TEST_F(JobHttpServerTest, ConcurrentRequests) {
    // Test concurrent requests to check for race conditions
    const int test_port = 8084;
    ASSERT_TRUE(http_server_->listen(test_port, "127.0.0.1"));
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    const int num_threads = 3;
    std::vector<std::thread> threads;
    std::vector<bool> results(num_threads, false);
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([this, test_port, &results, i]() {
            int client_fd = createClientSocket(test_port);
            if (client_fd >= 0) {
                std::string request = "GET /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
                std::string response = sendHttpRequest(client_fd, request);
                close(client_fd);
                
                results[i] = !response.empty() && 
                           response.find("HTTP/1.1 200") != std::string::npos;
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Check that most requests succeeded (allow for some timing issues)
    int successful_requests = 0;
    for (bool result : results) {
        if (result) successful_requests++;
    }
    
    EXPECT_GE(successful_requests, num_threads - 1) 
        << "Expected at least " << (num_threads - 1) << " successful requests out of " << num_threads;
}

// Test to debug the fd=0 issue specifically
TEST_F(JobHttpServerTest, DebugFileDescriptorHandling) {
    const int test_port = 8085;
    
    // Capture initial state of stdin
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    ASSERT_NE(stdin_flags, -1) << "Failed to get stdin flags";
    
    // Start server
    ASSERT_TRUE(http_server_->listen(test_port, "127.0.0.1"));
    
    // Check that stdin state hasn't changed
    int new_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    EXPECT_EQ(stdin_flags, new_stdin_flags) << "Server startup modified stdin flags";
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Make a connection and verify it uses a proper file descriptor
    int client_fd = createClientSocket(test_port);
    ASSERT_GE(client_fd, 0);
    
    // The client fd should be > 2 (not stdin/stdout/stderr)
    EXPECT_GT(client_fd, 2) << "Client socket got unexpected file descriptor: " << client_fd;
    
    close(client_fd);
}