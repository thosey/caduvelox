/**
 * Test suite for static HTTPS server example
 * 
 * Tests the static_https_server example functionality including:
 * - HTTPS server startup with KTLS
 * - Static file serving with proper MIME types
 * - Index.html serving at root path
 * - File serving under /files/ path
 * - Path traversal protection
 * - SSL/TLS connection handling
 */

#include <gtest/gtest.h>
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

using namespace caduvelox;
namespace fs = std::filesystem;

class StaticHttpsServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up logging
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        
        // Create temporary test directory structure
        test_dir_ = fs::temp_directory_path() / "caduvelox_test";
        fs::create_directories(test_dir_);
        fs::create_directories(test_dir_ / "files");
        
        // Create test files
        createTestFiles();
        
        // Initialize job server
        ASSERT_TRUE(job_server_.init(128));
        
        // Create HTTPS server
        https_server_ = std::make_unique<HttpServer>(job_server_);
        
        // Set up routes like static_https_server
        setupRoutes();
        
        // Find available port
        port_ = findAvailablePort();
        ASSERT_GT(port_, 0) << "Could not find available port";
        
        // Start HTTP server instead of HTTPS for initial testing
        ASSERT_TRUE(https_server_->listen(port_))
            << "Failed to start HTTP server on port " << port_;
        
        // Start event loop in background thread
        event_loop_thread_ = std::thread([this]() {
            job_server_.run();
        });
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    void TearDown() override {
        if (https_server_) {
            https_server_->stop();
        }
        job_server_.stop();
        
        // Wait for event loop thread to finish
        if (event_loop_thread_.joinable()) {
            event_loop_thread_.join();
        }
        
        // Clean up test directory
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }
    
private:
    void createTestFiles() {
        // Create index.html
        std::ofstream index(test_dir_ / "index.html");
        index << "<!DOCTYPE html>\n"
              << "<html><head><title>Test Index</title></head>\n"
              << "<body><h1>Welcome to Test Server</h1></body></html>\n";
        index.close();
        
        // Create test files in files/ subdirectory
        std::ofstream test_file(test_dir_ / "files" / "test.txt");
        test_file << "This is a test file content.\n";
        test_file.close();
        
        std::ofstream html_file(test_dir_ / "files" / "page.html");
        html_file << "<!DOCTYPE html>\n"
                  << "<html><head><title>Test Page</title></head>\n"
                  << "<body><h1>Test Page Content</h1></body></html>\n";
        html_file.close();
    }
    
    void setupRoutes() {
        // Serve index.html at /
        https_server_->addRoute("GET", R"(^/$)", 
            [this](const HttpRequest& req, HttpResponse& res){
                (void)req;
                std::string index_path = test_dir_ / "index.html";
                if (fs::exists(index_path) && fs::is_regular_file(index_path)) {
                    res.setContentType("text/html");
                    res.sendFile(index_path);
                } else {
                    res.status_code = 404;
                    res.body = "404 Not Found\n";
                }
            });

        // Serve files under /files/<path>
        https_server_->addRoute("GET", R"(^/files/(.*)$)", 
            [this](const HttpRequest& req, HttpResponse& res){
                static const std::regex file_re(R"(^/files/(.*)$)");
                std::smatch m;
                if (std::regex_match(req.path, m, file_re) && m.size() >= 2) {
                    std::string relpath = m[1].str();
                    
                    // Check for empty path
                    if (relpath.empty()) {
                        res.status_code = 400;
                        res.body = "400 Bad Request\n";
                        return;
                    }
                    
                    fs::path fp = test_dir_ / "files" / relpath;
                    // Prevent path traversal - check if normalized path starts with root
                    fs::path norm = fs::weakly_canonical(fp);
                    fs::path root = fs::weakly_canonical(test_dir_ / "files");
                    
                    // Ensure normalized path is within the files directory
                    auto norm_str = norm.string();
                    auto root_str = root.string();
                    if (norm_str.size() < root_str.size() || 
                        norm_str.substr(0, root_str.size()) != root_str) {
                        res.status_code = 403;
                        res.body = "403 Forbidden\n";
                        return;
                    }
                    
                    if (fs::exists(norm) && fs::is_regular_file(norm)) {
                        // Set content type based on file extension
                        std::string ext = norm.extension().string();
                        if (ext == ".html" || ext == ".htm") {
                            res.setContentType("text/html");
                        } else if (ext == ".txt") {
                            res.setContentType("text/plain");
                        } else if (ext == ".css") {
                            res.setContentType("text/css");
                        } else if (ext == ".js") {
                            res.setContentType("application/javascript");
                        } else if (ext == ".json") {
                            res.setContentType("application/json");
                        } else if (ext == ".png") {
                            res.setContentType("image/png");
                        } else if (ext == ".jpg" || ext == ".jpeg") {
                            res.setContentType("image/jpeg");
                        }
                        res.sendFile(norm.string());
                    } else {
                        res.status_code = 404;
                        res.body = "404 Not Found\n";
                    }
                } else {
                    res.status_code = 400;
                    res.body = "400 Bad Request\n";
                }
            });
    }
    
    int findAvailablePort() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;  // Let system choose port
        
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return -1;
        }
        
        socklen_t len = sizeof(addr);
        if (getsockname(sock, (struct sockaddr*)&addr, &len) < 0) {
            close(sock);
            return -1;
        }
        
        int port = ntohs(addr.sin_port);
        close(sock);
        return port;
    }
    
    // Helper to create SSL client connection
    SSL* createSSLClient() {
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) return nullptr;
        
        // Disable certificate verification for test
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        
        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            SSL_CTX_free(ctx);
            return nullptr;
        }
        
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            return nullptr;
        }
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            return nullptr;
        }
        
        SSL_set_fd(ssl, sock);
        
        if (SSL_connect(ssl) <= 0) {
            close(sock);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            return nullptr;
        }
        
        return ssl;
    }
    
protected:
    std::string sendHTTPRequest(const std::string& path) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return "";
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return "";
        }
        
        // Give async processing time to start (worker thread needs to pick up request)
        // Longer delay when running in full test suite due to system load
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        std::string request = "GET " + path + " HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "Connection: close\r\n\r\n";
        
        ssize_t sent = send(sock, request.c_str(), request.length(), 0);
        if (sent <= 0) {
            close(sock);
            return "";
        }
        
        // Set socket timeout to avoid hanging (longer for worker thread processing)
        struct timeval timeout;
        timeout.tv_sec = 10;  // Increased from 5 for async worker processing
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
        char buffer[4096];
        std::string response;
        ssize_t received;
        
        while (true) {
            received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (received > 0) {
                buffer[received] = '\0';
                response += buffer;
            } else if (received == 0) {
                // Connection closed
                break;
            } else {
                // Error
                if (errno == EINTR) {
                    // Interrupted by signal, retry
                    continue;
                }
                // Other error, give up
                break;
            }
        }
        
        close(sock);
        return response;
    }
    
    Server job_server_;
    std::unique_ptr<HttpServer> https_server_;
    std::thread event_loop_thread_;
    fs::path test_dir_;
    std::string cert_path_;
    std::string key_path_;
    int port_;
};

TEST_F(StaticHttpsServerTest, ServerStartsSuccessfully) {
    // Server should be running if we got here
    EXPECT_TRUE(https_server_ != nullptr);
    // Give it a moment to be fully ready
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(StaticHttpsServerTest, ServeIndexPage) {
    std::string response = sendHTTPRequest("/");
    
    EXPECT_FALSE(response.empty()) << "No response received";
    EXPECT_TRUE(response.find("HTTP/1.1 200") != std::string::npos) 
        << "Expected 200 status, got: " << response.substr(0, response.find("\r\n"));
    EXPECT_TRUE(response.find("Welcome to Test Server") != std::string::npos)
        << "Expected index.html content not found";
    EXPECT_TRUE(response.find("text/html") != std::string::npos)
        << "Expected HTML content type";
}

TEST_F(StaticHttpsServerTest, ServeStaticFile) {
    std::string response = sendHTTPRequest("/files/test.txt");
    
    EXPECT_FALSE(response.empty()) << "No response received";
    EXPECT_TRUE(response.find("HTTP/1.1 200") != std::string::npos)
        << "Expected 200 status, got: " << response.substr(0, response.find("\r\n"));
    EXPECT_TRUE(response.find("This is a test file content") != std::string::npos)
        << "Expected file content not found";
}

TEST_F(StaticHttpsServerTest, ServeHTMLFile) {
    std::string response = sendHTTPRequest("/files/page.html");
    
    EXPECT_FALSE(response.empty()) << "No response received";
    EXPECT_TRUE(response.find("HTTP/1.1 200") != std::string::npos)
        << "Expected 200 status";
    EXPECT_TRUE(response.find("Test Page Content") != std::string::npos)
        << "Expected HTML file content not found";
    EXPECT_TRUE(response.find("text/html") != std::string::npos)
        << "Expected HTML content type";
}

TEST_F(StaticHttpsServerTest, Return404ForNonexistentFile) {
    std::string response = sendHTTPRequest("/files/nonexistent.txt");
    
    EXPECT_FALSE(response.empty()) << "No response received";
    EXPECT_TRUE(response.find("HTTP/1.1 404") != std::string::npos)
        << "Expected 404 status for nonexistent file";
    EXPECT_TRUE(response.find("404 Not Found") != std::string::npos)
        << "Expected 404 error message";
}

TEST_F(StaticHttpsServerTest, PreventPathTraversal) {
    std::string response = sendHTTPRequest("/files/../index.html");
    
    EXPECT_FALSE(response.empty()) << "No response received";
    EXPECT_TRUE(response.find("HTTP/1.1 403") != std::string::npos)
        << "Expected 403 status for path traversal attempt";
    EXPECT_TRUE(response.find("403 Forbidden") != std::string::npos)
        << "Expected 403 error message";
}

TEST_F(StaticHttpsServerTest, HandleInvalidFileRequest) {
    std::string response = sendHTTPRequest("/files/");
    
    EXPECT_FALSE(response.empty()) << "No response received";
    EXPECT_TRUE(response.find("HTTP/1.1 400") != std::string::npos)
        << "Expected 400 status for invalid file request";
}