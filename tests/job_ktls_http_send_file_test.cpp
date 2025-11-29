#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <optional>
#include <string>
#include <cstring>
#include <filesystem>
#include <fstream>

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
    std::transform(headers_lower.begin(), headers_lower.end(), headers_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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

class JobKTLSSendFileTest : public ::testing::Test {
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
    }

    void TearDown() override {
        if (https_server) {
            https_server->stop();
        }
        job_server.stop();
        
        if (server_thread.joinable()) {
            server_thread.join();
        }
        
        // Clean up test file
        if (std::filesystem::exists(test_file_path)) {
            std::filesystem::remove(test_file_path);
        }
        
        // Give additional time for cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void createTestFile(const std::string& content) {
        test_file_path = "./job_ktls_test_file.bin";
        std::ofstream ofs(test_file_path, std::ios::binary);
        ofs.write(content.data(), content.size());
        ofs.close();
    }

    void startServer() {
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
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    static constexpr uint16_t BASE_PORT = 9800;
    static int test_counter;

    caduvelox::Server job_server;
    std::unique_ptr<caduvelox::SingleRingHttpServer> https_server;
    std::thread server_thread;
    uint16_t test_port;
    std::string test_file_path;
};

int JobKTLSSendFileTest::test_counter = 0;

TEST_F(JobKTLSSendFileTest, ServesStaticFileViaJobKTLS) {
    // Prepare a test file to serve
    std::string payload(128 * 1024, '\n'); // 128 KiB
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = char('A' + (i % 26));
    }
    createTestFile(payload);
    
    // Add route that serves the file using zero-copy
    https_server->addRoute("GET", "^/file$", [this](const caduvelox::HttpRequest& req, caduvelox::HttpResponse& res){
        (void)req;
        res.setContentType("application/octet-stream");
        res.sendFile(test_file_path); // Signal zero-copy file serving
    });
    
    startServer();

    // Client: connect and request file
    TLSClient client("127.0.0.1", test_port);
    std::string req = "GET /file HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(client.sendAll(req), (int)req.size());

    std::string buf; 
    buf.reserve(payload.size() + 4096);
    char tmp[16384];
    std::optional<ParsedResponse> parsed;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    
    while (std::chrono::steady_clock::now() < deadline) {
        int n = client.recvSome(tmp, sizeof(tmp));
        ASSERT_GE(n, 0);
        if (n == 0) break;
        buf.append(tmp, tmp + n);
        parsed = tryParseHttpResponse(buf);
        if (parsed) {
            if (buf.size() >= parsed->headers.size() + parsed->content_length) {
                break;
            }
        }
    }
    
    ASSERT_TRUE(parsed.has_value()) << "Failed to parse HTTP response";
    EXPECT_EQ(parsed->status_code, 200) << "Wrong status code";
    EXPECT_EQ(parsed->content_length, payload.size()) << "Content length mismatch";
    
    // Verify file content
    ASSERT_EQ(parsed->body.size(), payload.size()) << "Body size mismatch";
    EXPECT_EQ(parsed->body[0], payload[0]) << "First byte mismatch";
    EXPECT_EQ(parsed->body[parsed->body.size()-1], payload[payload.size()-1]) << "Last byte mismatch";
    
    // Verify a few more bytes to ensure content integrity
    for (size_t i = 0; i < std::min(payload.size(), static_cast<size_t>(1000)); i += 100) {
        EXPECT_EQ(parsed->body[i], payload[i]) << "Content mismatch at position " << i;
    }
}

TEST_F(JobKTLSSendFileTest, ServesMultipleFilesViaKTLS) {
    // Create two different test files
    std::string file1_content(64 * 1024, 'X'); // 64 KiB of X's
    std::string file2_content(32 * 1024, 'Y'); // 32 KiB of Y's
    
    std::string file1_path = "./job_ktls_test_file1.bin";
    std::string file2_path = "./job_ktls_test_file2.bin";
    
    {
        std::ofstream ofs1(file1_path, std::ios::binary);
        ofs1.write(file1_content.data(), file1_content.size());
    }
    {
        std::ofstream ofs2(file2_path, std::ios::binary);
        ofs2.write(file2_content.data(), file2_content.size());
    }
    
    // Add routes for both files
    https_server->addRoute("GET", "^/file1$", [file1_path](const caduvelox::HttpRequest& req, caduvelox::HttpResponse& res){
        (void)req;
        res.setContentType("application/octet-stream");
        res.sendFile(file1_path);
    });
    
    https_server->addRoute("GET", "^/file2$", [file2_path](const caduvelox::HttpRequest& req, caduvelox::HttpResponse& res){
        (void)req;
        res.setContentType("application/octet-stream");
        res.sendFile(file2_path);
    });
    
    startServer();
    
    // Test file1
    {
        TLSClient client("127.0.0.1", test_port);
        std::string req = "GET /file1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ASSERT_EQ(client.sendAll(req), (int)req.size());
        
        std::string buf;
        buf.reserve(file1_content.size() + 4096);
        char tmp[16384];
        std::optional<ParsedResponse> parsed;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        
        while (std::chrono::steady_clock::now() < deadline) {
            int n = client.recvSome(tmp, sizeof(tmp));
            ASSERT_GE(n, 0);
            if (n == 0) break;
            buf.append(tmp, tmp + n);
            parsed = tryParseHttpResponse(buf);
            if (parsed && buf.size() >= parsed->headers.size() + parsed->content_length) {
                break;
            }
        }
        
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->status_code, 200);
        EXPECT_EQ(parsed->content_length, file1_content.size());
        EXPECT_EQ(parsed->body.size(), file1_content.size());
        EXPECT_EQ(parsed->body[0], 'X');
        EXPECT_EQ(parsed->body[parsed->body.size()-1], 'X');
    }
    
    // Test file2
    {
        TLSClient client("127.0.0.1", test_port);
        std::string req = "GET /file2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ASSERT_EQ(client.sendAll(req), (int)req.size());
        
        std::string buf;
        buf.reserve(file2_content.size() + 4096);
        char tmp[16384];
        std::optional<ParsedResponse> parsed;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        
        while (std::chrono::steady_clock::now() < deadline) {
            int n = client.recvSome(tmp, sizeof(tmp));
            ASSERT_GE(n, 0);
            if (n == 0) break;
            buf.append(tmp, tmp + n);
            parsed = tryParseHttpResponse(buf);
            if (parsed && buf.size() >= parsed->headers.size() + parsed->content_length) {
                break;
            }
        }
        
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->status_code, 200);
        EXPECT_EQ(parsed->content_length, file2_content.size());
        EXPECT_EQ(parsed->body.size(), file2_content.size());
        EXPECT_EQ(parsed->body[0], 'Y');
        EXPECT_EQ(parsed->body[parsed->body.size()-1], 'Y');
    }
    
    // Clean up additional files
    std::filesystem::remove(file1_path);
    std::filesystem::remove(file2_path);
}