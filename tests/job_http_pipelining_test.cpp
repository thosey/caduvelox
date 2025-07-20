#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <sstream>
#include <optional>
#include <errno.h>

#include "caduvelox/Server.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"

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
    // Expect HTTP/1.1 XXX ...
    if (status_line.size() < 12) return std::nullopt;
    res.status_code = std::atoi(status_line.substr(9, 3).c_str());

    // Headers section
    res.headers = buffer.substr(0, header_end + 4);

    // Content-Length (case-insensitive)
    std::string headers_lower = res.headers;
    for (auto &c : headers_lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    const std::string cl_key = "content-length:";
    size_t cl_pos = headers_lower.find(cl_key);
    if (cl_pos == std::string::npos) return std::nullopt;
    size_t len_start = cl_pos + cl_key.size();
    // skip spaces
    while (len_start < res.headers.size() && (res.headers[len_start] == ' ' || res.headers[len_start] == '\t')) ++len_start;
    // len_start refers to the same index in the original headers string
    size_t len_end = res.headers.find("\r\n", len_start);
    if (len_end == std::string::npos) return std::nullopt;
    std::string len_str = res.headers.substr(len_start, len_end - len_start);
    res.content_length = static_cast<size_t>(std::strtoul(len_str.c_str(), nullptr, 10));

    size_t body_start = header_end + 4;
    if (buffer.size() - body_start < res.content_length) return std::nullopt; // need more

    res.body = buffer.substr(body_start, res.content_length);
    return res;
}

}

class JobHttpPipeliningTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up logging
        static caduvelox::ConsoleLogger console_logger;
        caduvelox::Logger::setGlobalLogger(&console_logger);
        
        test_port = BASE_PORT + test_counter++;
        
        // Initialize job server
        ASSERT_TRUE(job_server.init(128));
        
        // Create HTTP server
        http_server = std::make_unique<caduvelox::HttpServer>(job_server);
        
        // Add routes
        http_server->addRoute("GET", "^/r1$", [](const caduvelox::HttpRequest& req, caduvelox::HttpResponse& res){
            (void)req;
            res.status_code = 200;
            res.headers["content-type"] = "text/plain";
            res.body = "one";
        });
        
        http_server->addRoute("GET", "^/r2$", [](const caduvelox::HttpRequest& req, caduvelox::HttpResponse& res){
            (void)req;
            res.status_code = 200;
            res.headers["content-type"] = "text/plain";
            res.body = "two";
        });
        
        // Start server
        ASSERT_TRUE(http_server->listen(test_port, "127.0.0.1"));
        
        // Start event loop
        server_thread = std::thread([this]() {
            try {
                job_server.run();
            } catch (const std::exception& e) {
                std::cerr << "Server error: " << e.what() << std::endl;
            }
        });
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (http_server) {
            http_server->stop();
        }
        job_server.stop();
        
        if (server_thread.joinable()) {
            server_thread.join();
        }
        
        // Give additional time for cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    int connectClient() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(test_port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        int rc = ::connect(fd, (sockaddr*)&addr, sizeof(addr));
        EXPECT_EQ(rc, 0);
        return fd;
    }

    static constexpr uint16_t BASE_PORT = 9600;
    static int test_counter;

    caduvelox::Server job_server;
    std::unique_ptr<caduvelox::HttpServer> http_server;
    std::thread server_thread;
    uint16_t test_port;
};

int JobHttpPipeliningTest::test_counter = 0;

TEST_F(JobHttpPipeliningTest, TwoBackToBackRequestsOnSameConnection) {
    int fd = connectClient();

    // Set small recv timeout so test doesn't hang
    timeval tv{ .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string req1 = "GET /r1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string req2 = "GET /r2 HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // Send both requests without waiting for a response (HTTP/1.1 pipelining)
    std::string pipelined = req1 + req2;
    ssize_t sent = ::send(fd, pipelined.data(), pipelined.size(), 0);
    ASSERT_EQ(sent, static_cast<ssize_t>(pipelined.size()));

    std::string buf;
    buf.reserve(4096);

    // Read until we can parse first response
    char tmp[2048];
    ParsedResponse r1{};
    while (true) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EINTR) continue; // Retry on interrupt
            FAIL() << "recv error: " << strerror(errno) << " (errno=" << errno << ")";
        }
        if (n == 0) break; // closed
        buf.append(tmp, tmp + n);
        std::cerr << "[client] recv() returned " << n << " bytes, buf.size()=" << buf.size() << "\n";
        if (buf.size() > 0) {
            std::string snippet = buf.substr(0, std::min<size_t>(200, buf.size()));
            std::cerr << "[client] buf snippet: '" << snippet << "'\n";
        }
        auto parsed = tryParseHttpResponse(buf);
        if (parsed) {
            r1 = *parsed;
            break;
        }
    }

    ASSERT_EQ(r1.status_code, 200);
    ASSERT_EQ(r1.body, "one");

    // Remove first response from buffer and continue parsing second
    buf.erase(0, r1.headers.size() + r1.body.size());

    ParsedResponse r2{};
    auto second_start = std::chrono::steady_clock::now();
    const auto second_deadline = second_start + std::chrono::seconds(2);
    while (true) {
        auto parsed = tryParseHttpResponse(buf);
        if (parsed) {
            r2 = *parsed;
            break;
        }
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available yet, wait a bit and retry; fail if past deadline
                if (std::chrono::steady_clock::now() > second_deadline) {
                    FAIL() << "timeout waiting for second response (recv returned EAGAIN repeatedly)";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            FAIL() << "recv error on second response: " << strerror(errno) << " (errno=" << errno << ")";
        }
        if (n == 0) break;
        buf.append(tmp, tmp + n);
    }

    ASSERT_EQ(r2.status_code, 200);
    ASSERT_EQ(r2.body, "two");

    ::close(fd);
}

TEST_F(JobHttpPipeliningTest, MultipleSequentialRequests) {
    int fd = connectClient();

    // Set recv timeout
    timeval tv{ .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Send 5 pipelined requests
    std::string requests = 
        "GET /r1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /r2 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /r1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /r2 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /r1 HTTP/1.1\r\nHost: localhost\r\n\r\n";

    ssize_t sent = ::send(fd, requests.data(), requests.size(), 0);
    ASSERT_EQ(sent, static_cast<ssize_t>(requests.size()));

    // Expected responses
    std::vector<std::string> expected_bodies = {"one", "two", "one", "two", "one"};
    std::string buf;
    buf.reserve(8192);

    for (size_t i = 0; i < expected_bodies.size(); ++i) {
        ParsedResponse response{};
        auto start_time = std::chrono::steady_clock::now();
        const auto deadline = start_time + std::chrono::seconds(2);
        
        while (true) {
            auto parsed = tryParseHttpResponse(buf);
            if (parsed) {
                response = *parsed;
                break;
            }
            
            char tmp[2048];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n < 0) {
                if (errno == EINTR) continue; // Retry on interrupt
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        FAIL() << "timeout waiting for response " << (i + 1);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                FAIL() << "recv error on response " << (i + 1) << ": " << strerror(errno);
            }
            if (n == 0) {
                FAIL() << "connection closed while expecting response " << (i + 1);
            }
            buf.append(tmp, tmp + n);
        }

        ASSERT_EQ(response.status_code, 200) << "Response " << (i + 1) << " status code";
        ASSERT_EQ(response.body, expected_bodies[i]) << "Response " << (i + 1) << " body";

        // Remove this response from buffer
        buf.erase(0, response.headers.size() + response.body.size());
    }

    ::close(fd);
}