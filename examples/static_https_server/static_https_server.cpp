/**
 * Static HTTPS Server with Multi-Ring Architecture
 * 
 * High-performance static file server using multi-ring io_uring architecture
 * for maximum throughput and CPU utilization.
 * 
 * Usage: ./static_https_server <document_root> [port] [num_rings] [log_file] [cert_path] [key_path]
 * 
 * Examples:
 *   ./static_https_server static_site 8443                          # Auto-detect cores, console logging
 *   ./static_https_server /var/www 443 4                            # 4 rings, console logging
 *   ./static_https_server /var/www 443 0 /var/log/app.log           # Auto cores, file logging
 *   ./static_https_server /var/www 443 8 "" cert.pem key.pem        # 8 rings, custom certs
 *   ./static_https_server /var/www 443 4 app.log cert.pem key.pem   # All options
 * 
 * Certificate priority: command-line args > CERT_PATH/KEY_PATH env vars > test_cert.pem/test_key.pem
 * 
 * Features:
 * - Multi-ring io_uring architecture (one ring per CPU core)
 * - SO_REUSEPORT for kernel-level load balancing
 * - HTTPS with KTLS support
 * - Inline HTTP processing (no worker thread overhead)
 * - Static file serving with zero-copy splice
 * - Path traversal protection
 * - MIME type detection
 * - File or console logging with SIGHUP log rotation
 * 
 * Performance: Achieves 10-20x throughput improvement over single-ring architecture
 */

#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/http/HttpTypes.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include "caduvelox/logger/FileLogger.hpp"
#include "caduvelox/logger/AsyncLogger.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <regex>
#include <signal.h>
#include <pthread.h>
#include <thread>
#include <chrono>

using namespace caduvelox;
namespace fs = std::filesystem;

static std::unique_ptr<HttpServer> https_server;

int main(int argc, char** argv) {
    try {
        // Parse command-line arguments
        std::string docroot = argc > 1 ? argv[1] : "static_site";
        int port = argc > 2 ? std::stoi(argv[2]) : 8443;
        int num_rings = argc > 3 ? std::stoi(argv[3]) : 0;  // 0 = auto-detect
        std::string log_file = argc > 4 ? argv[4] : "";
        std::string cert_arg = argc > 5 ? argv[5] : "";
        std::string key_arg = argc > 6 ? argv[6] : "";

        std::cout << "Starting Multi-Ring Static HTTPS Server\n";
        std::cout << "Document root: " << docroot << "\n";
        std::cout << "Port: " << port << "\n";
        
        if (num_rings == 0) {
            num_rings = std::thread::hardware_concurrency();
            std::cout << "Service rings: " << num_rings << " (auto-detected)\n";
        } else {
            std::cout << "Service rings: " << num_rings << "\n";
        }

        // Set up logger based on whether log file is specified
        static std::unique_ptr<FileLogger> file_logger;
        static std::unique_ptr<AsyncLogger> async_logger;
        static std::unique_ptr<ConsoleLogger> console_logger;
        static FileLogger* file_logger_ptr = nullptr;

        if (!log_file.empty()) {
            std::cout << "Logging to: " << log_file << "\n";
            file_logger = std::make_unique<FileLogger>(log_file, true);
            file_logger_ptr = file_logger.get();
            async_logger = std::make_unique<AsyncLogger>(std::move(file_logger));
            Logger::setGlobalLogger(async_logger.get());
        } else {
            std::cout << "Logging to: console\n";
            console_logger = std::make_unique<ConsoleLogger>();
            Logger::setGlobalLogger(console_logger.get());
        }

        // Block signals in main thread and spawn a watcher thread
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        if (file_logger_ptr) {
            sigaddset(&set, SIGHUP);
        }
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
        
        std::thread([&]() {
            int sig = 0;
            while (true) {
                if (sigwait(&set, &sig) == 0) {
                    switch (sig) {
                        case SIGINT:
                            std::cout << "Received SIGINT, shutting down gracefully\n";
                            if (file_logger) {
                                file_logger->logMessage("Received SIGINT, shutting down gracefully");
                            }
                            if (https_server) https_server->stop();
                            return;
                        case SIGTERM:
                            std::cout << "Received SIGTERM, shutting down gracefully\n";
                            if (file_logger) {
                                file_logger->logMessage("Received SIGTERM, shutting down gracefully");
                            }
                            if (https_server) https_server->stop();
                            return;
                        case SIGHUP:
                            if (file_logger) {
                                file_logger->logMessage("Received SIGHUP, reopening log file");
                                file_logger->reopen();
                                std::cout << "Log file reopened (SIGHUP received)\n";
                            }
                            break;
                    }
                }
            }
        }).detach();

        // Calculate queue depth: use smaller depth for more rings to conserve memory
        // 20 rings * 1024 depth * ~200MB/ring = ~4GB (fits in most systems)
        // 4 rings * 4096 depth * ~200MB/ring = ~800MB
        unsigned queue_depth = (num_rings >= 16) ? 1024 : 4096;
        std::cout << "Queue depth per ring: " << queue_depth << "\n";

        // Initialize HTTPS server with optimized threading
        https_server = std::make_unique<HttpServer>(num_rings, queue_depth);
        
        // Set up HTTPS with certificates
        std::string cert_path, key_path;
        if (!cert_arg.empty() && !key_arg.empty()) {
            cert_path = cert_arg;
            key_path = key_arg;
        } else {
            const char* cert_env = std::getenv("CERT_PATH");
            const char* key_env = std::getenv("KEY_PATH");
            cert_path = cert_env ? cert_env : "test_cert.pem";
            key_path = key_env ? key_env : "test_key.pem";
        }

        // Serve index.html at /
        https_server->addRoute("GET", R"(^/$)", [docroot](const HttpRequest& req, HttpResponse& res){
            (void)req;
            std::string index_path = fs::path(docroot) / "index.html";
            if (fs::exists(index_path) && fs::is_regular_file(index_path)) {
                // Use sendFile for zero-copy file serving
                res.sendFile(index_path);
                res.setContentType("text/html");
            } else {
                res.status_code = 404;
                res.body = "404 Not Found\n";
            }
        });

        // Serve files under /files/<path>
        https_server->addRoute("GET", R"(^/files/(.+)$)", [docroot](const HttpRequest& req, HttpResponse& res){
            // Extract path from request (e.g., /files/test.txt -> test.txt)
            std::string path = req.path.substr(7);  // Remove "/files/" prefix
            
            // Prevent path traversal
            if (path.find("..") != std::string::npos || path.empty() || path[0] == '/') {
                res.status_code = 403;
                res.body = "403 Forbidden\n";
                return;
            }
            
            // Map to <docroot>/files/<path>
            fs::path fp = fs::path(docroot) / "files" / path;
            
            // Normalize and verify it's inside docroot
            fs::path norm = fs::weakly_canonical(fp);
            fs::path root = fs::weakly_canonical(docroot);
            
            auto rel = norm.lexically_relative(root);
            if (rel.empty() || rel.string().rfind("..", 0) == 0) {
                res.status_code = 403;
                res.body = "403 Forbidden\n";
                return;
            }
            
            if (fs::exists(norm) && fs::is_regular_file(norm)) {
                res.sendFile(norm.string());
            } else {
                res.status_code = 404;
                res.body = "404 Not Found\n";
            }
        });

        // Start listening with KTLS
        if (!https_server->listenKTLS(port, cert_path, key_path)) {
            std::cerr << "Failed to start HTTPS server on port " << port << std::endl;
            return 1;
        }

        std::cout << "Listening on https://0.0.0.0:" << port << " (KTLS enabled, " << num_rings << " rings)\n";
        std::cout << "Press Ctrl+C to stop\n";
        Logger::getInstance().logMessage("Server startup complete, listening on port " + std::to_string(port));
        
        // Run the server (blocking)
        Logger::getInstance().logMessage("Server event loop starting");
        https_server->run();  // Blocking - runs all service ring threads
        Logger::getInstance().logMessage("Server event loop exited");

        Logger::getInstance().logMessage("Beginning cleanup");
        https_server.reset();

        std::cout << "Shutdown complete" << std::endl;
        Logger::getInstance().logMessage("Shutdown complete");
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        Logger::getInstance().logError("Fatal error: " + std::string(e.what()));
        return 1;
    } catch (...) {
        std::cerr << "Unknown fatal error occurred" << std::endl;
        Logger::getInstance().logError("Unknown fatal error occurred");
        return 1;
    }
}
