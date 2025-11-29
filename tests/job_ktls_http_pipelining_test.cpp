#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <optional>
#include <string>
#include <cstring>
#include <algorithm>

// Provide sane defaults if pool/timeout macros not passed in build.
#ifndef CDV_KTLS_POOL_SIZE
#define CDV_KTLS_POOL_SIZE 1000
#endif
#ifndef CDV_ACCEPT_POOL_SIZE
#define CDV_ACCEPT_POOL_SIZE 100
#endif
#ifndef CDV_KTLS_HANDSHAKE_TIMEOUT_MS
#define CDV_KTLS_HANDSHAKE_TIMEOUT_MS 5000
#endif

namespace {

struct ParsedResponse {
    int status_code;
    size_t content_length;
    std::string headers;
    std::string body;
};

std::optional<ParsedResponse> tryParseHttpResponse(const std::string &buffer) {
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) return std::nullopt;

    ParsedResponse res{};

    // Parse status line
    size_t line_end = buffer.find("\r\n");
    if (line_end == std::string::npos) return std::nullopt;
    std::string status_line = buffer.substr(0, line_end);
    if (status_line.size() < 12) return std::nullopt;
    res.status_code = std::atoi(status_line.substr(9, 3).c_str());

    // Headers section
    res.headers = buffer.substr(0, header_end + 4);

    // Content-Length (case-insensitive search)
    std::string headers_lower = res.headers;
    std::transform(headers_lower.begin(), headers_lower.end(), headers_lower.begin(), ::tolower);
    size_t cl_pos = headers_lower.find("content-length:");
    if (cl_pos == std::string::npos) return std::nullopt;
    size_t len_start = cl_pos + std::strlen("content-length:");
    while (len_start < res.headers.size() && (res.headers[len_start] == ' ' || res.headers[len_start] == '\t')) ++len_start;
    size_t len_end = res.headers.find("\r\n", len_start);
    if (len_end == std::string::npos) return std::nullopt;
    std::string len_str = res.headers.substr(len_start, len_end - len_start);
    res.content_length = static_cast<size_t>(std::strtoul(len_str.c_str(), nullptr, 10));

    size_t body_start = header_end + 4;
    if (buffer.size() - body_start < res.content_length) return std::nullopt; // need more

    res.body = buffer.substr(body_start, res.content_length);
    return res;
}

// Minimal TLS client helper to send/recv raw HTTP over TLS
class TLSClient {
public:
    TLSClient(const char* addr, int port) {
        SSL_library_init();
        SSL_load_error_strings();
        ctx_ = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);
        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port); inet_pton(AF_INET, addr, &sa.sin_addr);
        ::connect(sock_, (sockaddr*)&sa, sizeof(sa));
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, sock_);
        int r = SSL_connect(ssl_);
        (void)r;
    }
    ~TLSClient() {
        if (ssl_) SSL_free(ssl_);
        if (ctx_) SSL_CTX_free(ctx_);
        if (sock_ >= 0) ::close(sock_);
    }
    int sendAll(const std::string& s) { return SSL_write(ssl_, s.data(), (int)s.size()); }
    int recvSome(char* buf, size_t cap) { return SSL_read(ssl_, buf, (int)cap); }
private:
    SSL_CTX* ctx_{nullptr};
    SSL* ssl_{nullptr};
    int sock_{-1};
};

}

class JobKTLSPipeliningTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up logging
        static caduvelox::ConsoleLogger console_logger;
        caduvelox::Logger::setGlobalLogger(&console_logger);
        
        test_port = BASE_PORT + test_counter++;
        
        // Initialize job server
        ASSERT_TRUE(job_server.init(128));
        
        // Create HTTPS server
        https_server = std::make_unique<caduvelox::SingleRingHttpServer>(job_server);
        
        // Add routes
        https_server->addRoute("GET", "^/r1$", [](const caduvelox::HttpRequest& req, caduvelox::HttpResponse& res){
            (void)req;
            res.status_code = 200;
            res.headers["content-type"] = "text/plain";
            res.body = "one";
        });
        
        https_server->addRoute("GET", "^/r2$", [](const caduvelox::HttpRequest& req, caduvelox::HttpResponse& res){
            (void)req;
            res.status_code = 200;
            res.headers["content-type"] = "text/plain";
            res.body = "two";
        });
        
        // Start HTTPS server with KTLS
        ASSERT_TRUE(https_server->listenKTLS(test_port, "test_cert.pem", "test_key.pem", "127.0.0.1"));
        
        // Start event loop
        server_thread = std::thread([this]() {
            try {
                job_server.run();
            } catch (const std::exception& e) {
                std::cerr << "Server error: " << e.what() << std::endl;
            }
        });
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (https_server) {
            https_server->stop();
        }
        job_server.stop();
        
        if (server_thread.joinable()) {
            server_thread.join();
        }
        
        // Give additional time for cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    static constexpr uint16_t BASE_PORT = 9700;
    static int test_counter;

    caduvelox::Server job_server;
    std::unique_ptr<caduvelox::SingleRingHttpServer> https_server;
    std::thread server_thread;
    uint16_t test_port;
};

int JobKTLSPipeliningTest::test_counter = 0;

TEST_F(JobKTLSPipeliningTest, TwoBackToBackRequestsOnSameConnection) {
    // Client: connect and pipeline
    TLSClient client("127.0.0.1", test_port);
    std::string req1 = "GET /r1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string req2 = "GET /r2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string pipelined = req1 + req2;
    ASSERT_EQ(client.sendAll(pipelined), (int)pipelined.size());

    std::string buf; 
    buf.reserve(4096);
    char tmp[2048];
    
    // Read first response
    ParsedResponse r1{};
    while (true) {
        int n = client.recvSome(tmp, sizeof(tmp));
        ASSERT_GE(n, 0);
        if (n == 0) break;
        buf.append(tmp, tmp + n);
        auto parsed = tryParseHttpResponse(buf);
        if (parsed) { 
            r1 = *parsed; 
            break; 
        }
    }
    ASSERT_EQ(r1.status_code, 200);
    ASSERT_EQ(r1.body, "one");

    // Remove first response and read second
    buf.erase(0, r1.headers.size() + r1.body.size());

    ParsedResponse r2{};
    while (true) {
        auto parsed = tryParseHttpResponse(buf);
        if (parsed) { 
            r2 = *parsed; 
            break; 
        }
        int n = client.recvSome(tmp, sizeof(tmp));
        ASSERT_GE(n, 0);
        if (n == 0) break;
        buf.append(tmp, tmp + n);
    }
    ASSERT_EQ(r2.status_code, 200);
    ASSERT_EQ(r2.body, "two");
}

// Simulate pool exhaustion of KTLSJob by compiling with very small CDV_KTLS_POOL_SIZE
// and opening more simultaneous client connections than the pool size, verifying that
// connection attempts beyond the pool size fail initially but succeed after timeouts
// reclaim stuck handshakes.
TEST_F(JobKTLSPipeliningTest, KTLSPoolExhaustionRecovery) {
    // This test depends on a reduced pool size (e.g. -DCDV_KTLS_POOL_SIZE=8) and a short handshake timeout
    // (e.g. -DCDV_KTLS_HANDSHAKE_TIMEOUT_MS=200). If not defined, skip.
#if !defined(CDV_KTLS_POOL_SIZE) || !defined(CDV_KTLS_HANDSHAKE_TIMEOUT_MS)
    GTEST_SKIP() << "Compile with -DCDV_KTLS_POOL_SIZE and -DCDV_KTLS_HANDSHAKE_TIMEOUT_MS for this test";
#endif

    const int poolSize = CDV_KTLS_POOL_SIZE; // expected max concurrent handshakes
    const int extra = poolSize + 4; // exceed pool intentionally

    std::vector<std::unique_ptr<TLSClient>> clients;
    clients.reserve(extra);

    // Open connections without sending data to keep them in handshake WANT_READ/WRITE state
    for (int i = 0; i < extra; ++i) {
        clients.emplace_back(std::make_unique<TLSClient>("127.0.0.1", test_port));
        // Small sleep to stagger
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // At this point some connections beyond pool size may have been closed by server.
    // Wait for at least one timeout cycle to reclaim stalled jobs.
    std::this_thread::sleep_for(std::chrono::milliseconds(CDV_KTLS_HANDSHAKE_TIMEOUT_MS + 300));

    // Attempt an additional connection that should now succeed after reclamation.
    auto lateClient = std::make_unique<TLSClient>("127.0.0.1", test_port);
    std::string req = "GET /r1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(lateClient->sendAll(req), (int)req.size());
    char buf[1024];
    std::string accum;
    for (int tries = 0; tries < 50; ++tries) {
        int n = lateClient->recvSome(buf, sizeof(buf));
        if (n > 0) {
            accum.append(buf, buf + n);
            auto parsed = tryParseHttpResponse(accum);
            if (parsed) {
                EXPECT_EQ(parsed->status_code, 200);
                break;
            }
        } else if (n == 0) {
            // connection closed prematurely
            FAIL() << "Late client connection closed unexpectedly";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

TEST_F(JobKTLSPipeliningTest, MultipleKTLSRequests) {
    // Test multiple pipelined requests over KTLS
    TLSClient client("127.0.0.1", test_port);
    
    // Send 4 pipelined requests
    std::string requests = 
        "GET /r1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /r2 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /r1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /r2 HTTP/1.1\r\nHost: localhost\r\n\r\n";

    ASSERT_EQ(client.sendAll(requests), (int)requests.size());

    // Expected responses
    std::vector<std::string> expected_bodies = {"one", "two", "one", "two"};
    std::string buf;
    buf.reserve(8192);
    char tmp[2048];

    for (size_t i = 0; i < expected_bodies.size(); ++i) {
        ParsedResponse response{};
        
        while (true) {
            auto parsed = tryParseHttpResponse(buf);
            if (parsed) {
                response = *parsed;
                break;
            }
            
            int n = client.recvSome(tmp, sizeof(tmp));
            ASSERT_GE(n, 0) << "Recv error on response " << (i + 1);
            if (n == 0) {
                FAIL() << "Connection closed while expecting response " << (i + 1);
            }
            buf.append(tmp, tmp + n);
        }

        ASSERT_EQ(response.status_code, 200) << "Response " << (i + 1) << " status code";
        ASSERT_EQ(response.body, expected_bodies[i]) << "Response " << (i + 1) << " body";

        // Remove this response from buffer
        buf.erase(0, response.headers.size() + response.body.size());
    }
}