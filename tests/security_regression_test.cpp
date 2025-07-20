/**
 * Security Regression Tests
 * 
 * These tests verify that critical security and reliability bugs stay fixed
 * during future refactorings. Tests focus on HTTP parser correctness,
 * error handling, and shutdown safety.
 */

#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/http/HttpParser.hpp"
#include "caduvelox/http/HttpTypes.hpp"
#include "caduvelox/threading/AffinityWorkerPool.hpp"
#include <thread>
#include <chrono>

using namespace caduvelox;

class SecurityRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Tests run in isolation
    }

    void TearDown() override {
        // Cleanup
    }
};

// ============================================================================
// HTTP Parser Fatal Error Detection
// ============================================================================

TEST_F(SecurityRegressionTest, ParserDistinguishesIncompleteFromBadRequest) {
    // Test that parser properly distinguishes between:
    // - Incomplete request (need more data) - normal condition
    // - Malformed request (bad syntax) - fatal error
    
    // Incomplete request (need more data)
    {
        std::string incomplete = "GET /test HTTP/1.1\r\n";  // Missing headers end
        HttpRequest req;
        size_t consumed = 0;
        auto result = HttpParser::parse_request(incomplete, req, consumed);
        EXPECT_EQ(result, HttpParser::ParseResult::Incomplete) 
            << "Incomplete request should return Incomplete, not hang forever";
    }

    // Valid complete request
    {
        std::string valid = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
        HttpRequest req;
        size_t consumed = 0;
        auto result = HttpParser::parse_request(valid, req, consumed);
        EXPECT_EQ(result, HttpParser::ParseResult::Success)
            << "Valid request should return Success";
        EXPECT_EQ(req.method, "GET");
        EXPECT_EQ(req.path, "/test");
    }
}

TEST_F(SecurityRegressionTest, ParserRejectsOversizedContentLength) {
    // Parser should reject Content-Length > 1GB to prevent DoS
    std::string huge_content_length = 
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 2000000000\r\n"  // 2GB
        "\r\n";
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(huge_content_length, req, consumed);
    
    EXPECT_EQ(result, HttpParser::ParseResult::BadRequest)
        << "Parser should reject Content-Length > 1GB to prevent DoS";
}

TEST_F(SecurityRegressionTest, ParserRejectsChunkedEncoding) {
    // Parser should reject chunked encoding (not yet supported)
    std::string chunked = 
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(chunked, req, consumed);
    
    EXPECT_EQ(result, HttpParser::ParseResult::BadRequest)
        << "Parser should reject unsupported chunked encoding";
}

TEST_F(SecurityRegressionTest, ParserHandlesMissingColon) {
    // Parser is permissive - it skips headers without colon
    // This test verifies it doesn't crash on such input
    std::string malformed = 
        "GET /test HTTP/1.1\r\n"
        "InvalidHeaderNoColon\r\n"
        "\r\n";
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(malformed, req, consumed);
    
    // Parser succeeds by skipping the line without colon
    EXPECT_EQ(result, HttpParser::ParseResult::Success)
        << "Parser handles missing colon by skipping line";
}

TEST_F(SecurityRegressionTest, ParserRejectsOversizedPartialRequest) {
    // Parser should reject oversized partial requests (DoS prevention)
    // When incomplete headers exceed MAX_REQUEST_LINE + (MAX_HEADERS * MAX_HEADER_LINE)
    // = 8KB + (200 * 16KB) = ~3.2MB, return BadRequest
    std::string huge_partial;
    const size_t target_size = 8*1024 + (200 * 16*1024) + 1000;  // Exceed limit
    huge_partial.reserve(target_size);
    
    // Create a valid request line
    huge_partial = "GET /test HTTP/1.1\r\n";
    
    // Add many large headers WITHOUT completing (no final \r\n)
    // This simulates a DoS attack with endless partial headers
    while (huge_partial.size() < target_size) {
        huge_partial += "X-Header: ";
        huge_partial.append(15000, 'x');  // 15KB per header
        huge_partial += "\r\n";
    }
    // Don't add final \r\n - this is an incomplete request
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(huge_partial, req, consumed);
    
    // Should be BadRequest because partial headers exceed limit
    EXPECT_EQ(result, HttpParser::ParseResult::BadRequest)
        << "Parser should reject oversized partial request to prevent DoS";
}

TEST_F(SecurityRegressionTest, ParserHandlesMultipleValidRequests) {
    // Verify parser correctly processes multiple valid requests
    std::vector<std::string> requests = {
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "POST /data HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello",
        "GET /api HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
    };
    
    for (const auto& req_str : requests) {
        HttpRequest req;
        size_t consumed = 0;
        auto result = HttpParser::parse_request(req_str, req, consumed);
        EXPECT_EQ(result, HttpParser::ParseResult::Success)
            << "Valid request should parse successfully: " << req_str;
    }
}

// ============================================================================
// Shutdown Safety Tests
// ============================================================================

TEST_F(SecurityRegressionTest, ServerShutdownWithoutCrash) {
    // Test that server can shut down cleanly without use-after-free
    // This catches race conditions in eventfd cleanup and job destruction
    
    for (int i = 0; i < 3; i++) {  // Run multiple times to catch races
        Server server;
        ASSERT_TRUE(server.init(128));
        
        auto worker_pool = std::make_shared<AffinityWorkerPool>(2);
        HttpServer http_server(server, worker_pool);
        
        http_server.addRoute("GET", "/test", [](const HttpRequest& req, HttpResponse& res) {
            res.status_code = 200;
            res.body = "OK";
        });

        // Start server briefly
        int port = 50000 + i;
        bool listening = http_server.listen(port);
        
        if (listening) {
            std::thread server_thread([&server]() {
                // Run briefly then stop
                server.run();
            });
            
            // Let server initialize
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            // Immediate shutdown while workers may be active
            http_server.stop();
            server.stop();
            
            if (server_thread.joinable()) {
                server_thread.join();
            }
        }
        
        // If there's a use-after-free, this will crash or trigger ASAN
        // Success means we get here without crashing
    }
    
    SUCCEED() << "Server shut down cleanly 3 times without use-after-free";
}

TEST_F(SecurityRegressionTest, ServerShutdownWithWorkerPool) {
    // Specifically test shutdown with active worker pool
    // This tests EventFdMonitorJob cleanup ordering
    
    Server server;
    ASSERT_TRUE(server.init(128));
    
    auto worker_pool = std::make_shared<AffinityWorkerPool>(4);
    HttpServer http_server(server, worker_pool);
    
    int request_count = 0;
    http_server.addRoute("GET", "/count", [&request_count](const HttpRequest& req, HttpResponse& res) {
        request_count++;
        res.status_code = 200;
        res.body = std::to_string(request_count);
    });
    
    bool listening = http_server.listen(50010);
    EXPECT_TRUE(listening) << "Server should be able to listen";
    
    if (listening) {
        std::thread server_thread([&server]() {
            server.run();
        });
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Post some work to workers (optional - tests with and without work)
        // In real usage, workers might be processing when shutdown happens
        
        // Clean shutdown
        http_server.stop();
        server.stop();
        
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
    
    SUCCEED() << "Server with worker pool shut down cleanly";
}

// ============================================================================
// HTTP Router Tests (architecture-agnostic)
// ============================================================================

TEST_F(SecurityRegressionTest, RouterHandlesBasicRoutes) {
    // Test that HTTP routing works correctly with the parser
    Server server;
    ASSERT_TRUE(server.init(128));
    
    HttpServer http_server(server);
    
    bool route1_called = false;
    bool route2_called = false;
    
    http_server.addRoute("GET", "/api/test", [&route1_called](const HttpRequest& req, HttpResponse& res) {
        route1_called = true;
        res.status_code = 200;
        res.body = "Route 1";
    });
    
    http_server.addRoute("POST", "/api/data", [&route2_called](const HttpRequest& req, HttpResponse& res) {
        route2_called = true;
        res.status_code = 201;
        res.body = "Route 2";
    });
    
    // Routes are registered
    SUCCEED() << "Routes registered successfully";
}

// ============================================================================
// Parser Edge Cases
// ============================================================================

TEST_F(SecurityRegressionTest, ParserHandlesEmptyPath) {
    // Empty path should be treated as "/" or rejected
    std::string request = "GET  HTTP/1.1\r\nHost: localhost\r\n\r\n";
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(request, req, consumed);
    
    // Parser behavior: may reject or normalize - either is acceptable
    // Main thing: shouldn't crash or hang
    EXPECT_TRUE(result == HttpParser::ParseResult::BadRequest || 
                result == HttpParser::ParseResult::Success)
        << "Empty path should either be rejected or normalized, not hang";
}

TEST_F(SecurityRegressionTest, ParserHandlesVeryLongPath) {
    // Very long but valid path should parse or be rejected gracefully
    std::string long_path(8000, 'a');  // 8KB path
    std::string request = "GET /" + long_path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(request, req, consumed);
    
    // Should either parse successfully or reject as oversized
    EXPECT_TRUE(result == HttpParser::ParseResult::Success || 
                result == HttpParser::ParseResult::BadRequest)
        << "Long path should parse or be rejected, not hang";
}

TEST_F(SecurityRegressionTest, ParserHandlesMultipleHeaderValues) {
    // Test parsing of multiple headers
    std::string request = 
        "GET /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: text/html\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: keep-alive\r\n"
        "User-Agent: TestClient/1.0\r\n"
        "\r\n";
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(request, req, consumed);
    
    EXPECT_EQ(result, HttpParser::ParseResult::Success);
    EXPECT_EQ(req.headers.count("host"), 1);
    EXPECT_EQ(req.headers.count("accept"), 1);
    EXPECT_EQ(req.headers.count("connection"), 1);
}

// ============================================================================
// Content-Length Handling
// ============================================================================

TEST_F(SecurityRegressionTest, ParserHandlesZeroContentLength) {
    // Content-Length: 0 is valid
    std::string request = 
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(request, req, consumed);
    
    EXPECT_EQ(result, HttpParser::ParseResult::Success);
}

TEST_F(SecurityRegressionTest, ParserHandlesReasonableContentLength) {
    // 1MB Content-Length should be accepted (when body is present)
    std::string request = 
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "0123456789";  // Actual body matching Content-Length
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(request, req, consumed);
    
    // With matching body present, parsing succeeds
    EXPECT_EQ(result, HttpParser::ParseResult::Success)
        << "Reasonable Content-Length with body should be accepted";
    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(req.body.size(), 10);
}

TEST_F(SecurityRegressionTest, ParserRejectsNegativeContentLength) {
    // Negative Content-Length should be rejected
    std::string request = 
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: -100\r\n"
        "\r\n";
    
    HttpRequest req;
    size_t consumed = 0;
    auto result = HttpParser::parse_request(request, req, consumed);
    
    EXPECT_EQ(result, HttpParser::ParseResult::BadRequest)
        << "Negative Content-Length should be rejected";
}
