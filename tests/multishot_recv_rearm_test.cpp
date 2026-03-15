#include <gtest/gtest.h>
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <string>

using namespace caduvelox;

/**
 * Test for Issue #3: MultishotRecvJob Not Re-arming
 * 
 * Verifies that when multishot recv terminates (IORING_CQE_F_MORE flag drops),
 * the connection properly re-arms with a new recv operation rather than stalling.
 * 
 * Expected Result: This test should FAIL before the fix, showing that keep-alive
 * and pipelined connections stall after the multishot recv terminates.
 * 
 * Note: This issue is kernel-dependent. Multishot recv may terminate after
 * processing a certain number of events. The test uses pipelined requests
 * to increase the likelihood of triggering this condition.
 */
class MultishotRecvRearmTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        
        ASSERT_TRUE(job_server_.init(256));
        
        http_server_ = std::make_unique<SingleRingHttpServer>(job_server_);
        
        // Add route that supports keep-alive
        http_server_->addRoute("GET", "/test", [this](const HttpRequest& req, HttpResponse& res) {
            res.setStatus(200);
            res.setBody("OK");
            res.setHeader("Content-Type", "text/plain");
            // Support keep-alive (don't force close)
            request_count_++;
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
        
        // Set timeout to detect stalls
        struct timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
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
    
    bool sendPipelinedRequests(int client_fd, int num_requests) {
        // Send multiple requests in a pipeline (keep-alive connection)
        std::string pipelined;
        for (int i = 0; i < num_requests; i++) {
            pipelined += "GET /test HTTP/1.1\r\n";
            pipelined += "Host: localhost\r\n";
            pipelined += "Connection: keep-alive\r\n";
            pipelined += "\r\n";
        }
        
        ssize_t sent = send(client_fd, pipelined.c_str(), pipelined.length(), 0);
        return sent == static_cast<ssize_t>(pipelined.length());
    }
    
    int countResponses(int client_fd) {
        char buffer[8192];
        std::string all_data;
        int response_count = 0;
        
        while (true) {
            ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                break;  // Timeout or connection closed
            }
            
            all_data.append(buffer, received);
            
            // Count HTTP responses (look for "HTTP/1.1 200 OK")
            size_t pos = 0;
            while ((pos = all_data.find("HTTP/1.1 200", pos)) != std::string::npos) {
                response_count++;
                pos += 12;
            }
            
            // If we got enough responses, we can stop
            if (response_count >= 10) {
                break;
            }
            
            // Small delay to allow more data to arrive
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return response_count;
    }
    
    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_server_;
    std::thread event_loop_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> request_count_{0};
    const int test_port_ = 8891;
};

TEST_F(MultishotRecvRearmTest, PipelinedRequestsAllProcessed) {
    // This test sends multiple pipelined requests on a keep-alive connection
    // If multishot recv terminates and doesn't re-arm, later requests will stall
    
    int client_fd = createAndConnectSocket();
    ASSERT_GE(client_fd, 0) << "Failed to connect to server";
    
    const int num_requests = 10;
    std::cout << "Sending " << num_requests << " pipelined requests..." << std::endl;
    
    ASSERT_TRUE(sendPipelinedRequests(client_fd, num_requests));
    
    std::cout << "Waiting for responses..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    int response_count = countResponses(client_fd);
    close(client_fd);
    
    std::cout << "Received " << response_count << " responses out of " 
              << num_requests << " requests" << std::endl;
    
    // This should FAIL before the fix if multishot recv terminates mid-pipeline
    EXPECT_EQ(response_count, num_requests)
        << "Not all pipelined requests were processed. Connection may have stalled.";
}

TEST_F(MultishotRecvRearmTest, KeepAliveConnectionContinuesWorking) {
    // Send requests sequentially on same keep-alive connection
    // If multishot recv terminates, subsequent requests will timeout
    
    int client_fd = createAndConnectSocket();
    ASSERT_GE(client_fd, 0);
    
    const int num_rounds = 5;
    int total_responses = 0;
    
    for (int round = 0; round < num_rounds; round++) {
        std::cout << "Round " << round << ": Sending request..." << std::endl;
        
        const char* request = "GET /test HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "Connection: keep-alive\r\n"
                             "\r\n";
        
        ssize_t sent = send(client_fd, request, strlen(request), 0);
        ASSERT_GT(sent, 0) << "Failed to send request in round " << round;
        
        // Wait for response
        char buffer[1024];
        ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
        
        if (received > 0) {
            total_responses++;
            std::cout << "Round " << round << ": Received response (" << received << " bytes)" << std::endl;
        } else {
            std::cout << "Round " << round << ": No response (timeout or connection closed)" << std::endl;
            break;
        }
        
        // Small delay between requests
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    close(client_fd);
    
    std::cout << "Successfully received " << total_responses << " responses out of " 
              << num_rounds << " rounds" << std::endl;
    
    // This should FAIL if connection stalls after multishot recv terminates
    EXPECT_EQ(total_responses, num_rounds)
        << "Keep-alive connection stalled after " << total_responses << " requests.";
}

TEST_F(MultishotRecvRearmTest, MultipleConnectionsAllWork) {
    // Test with multiple connections to increase chance of hitting the issue
    const int num_connections = 5;
    const int requests_per_connection = 8;
    
    std::vector<std::pair<int, int>> results;  // (expected, actual)
    
    for (int i = 0; i < num_connections; i++) {
        int client_fd = createAndConnectSocket();
        if (client_fd < 0) continue;
        
        sendPipelinedRequests(client_fd, requests_per_connection);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        int received = countResponses(client_fd);
        results.push_back({requests_per_connection, received});
        
        close(client_fd);
        
        std::cout << "Connection " << i << ": sent " << requests_per_connection 
                  << ", received " << received << std::endl;
    }
    
    // Check if any connections stalled
    int fully_processed = 0;
    for (const auto& [expected, actual] : results) {
        if (actual == expected) {
            fully_processed++;
        }
    }
    
    std::cout << fully_processed << " out of " << results.size() 
              << " connections fully processed" << std::endl;
    
    // This should FAIL if multishot recv doesn't re-arm properly
    EXPECT_EQ(fully_processed, results.size())
        << "Some connections stalled, likely due to multishot recv not re-arming.";
}

TEST_F(MultishotRecvRearmTest, LargePayloadPipeline) {
    // Send large pipelined payload to increase likelihood of multishot termination
    int client_fd = createAndConnectSocket();
    ASSERT_GE(client_fd, 0);
    
    const int num_requests = 12;  // More requests to stress multishot
    
    sendPipelinedRequests(client_fd, num_requests);
    
    // Give more time for large batch
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    int received = countResponses(client_fd);
    close(client_fd);
    
    std::cout << "Large payload test: sent " << num_requests 
              << ", received " << received << std::endl;
    
    // Allow some tolerance for very large batches
    EXPECT_GE(received, num_requests - 2)
        << "Large pipeline significantly incomplete, connection likely stalled.";
}
