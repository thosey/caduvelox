/**
 * Static HTTPS Server with Job-Based Architecture
 * 
 * Demonstrates serving static files over HTTPS using the job-based
 * architecture with zero-copy operations.
 * 
 * Usage: ./static_https_server <document_root> [port]
 * 
 * Features:
 * - HTTPS with KTLS support
 * - Static file serving with zero-copy
 * - Path traversal protection
 * - MIME type detection
 */

#include "caduvelox/Server.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/http/HttpTypes.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
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

static std::unique_ptr<Server> job_server;
static std::unique_ptr<HttpServer> https_server;

int main(int argc, char** argv) {
    try {
        // Usage: ./static_https_server [document_root] [port]
        std::string docroot = argc > 1 ? argv[1] : "static_site";
        int port = argc > 2 ? std::stoi(argv[2]) : 8443;

        std::cout << "Starting static HTTPS server (job-based)\n";
        std::cout << "Document root: " << docroot << "\n";
        std::cout << "Port: " << port << "\n";

        // Set up console logger
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        // Initialize job server
        job_server = std::make_unique<Server>();
        if (!job_server->init(256)) {
            std::cerr << "Failed to initialize Server" << std::endl;
            return 1;
        }

        // Block signals in main thread and spawn a watcher thread using sigwait
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
        std::thread([&]() {
            int sig = 0;
            if (sigwait(&set, &sig) == 0) {
                if (job_server) job_server->stop();
            }
        }).detach();

        // Initialize HTTPS server
        https_server = std::make_unique<HttpServer>(*job_server);
        
        // Set up HTTPS with certificates
        const char* cert_env = std::getenv("CERT_PATH");
        const char* key_env = std::getenv("KEY_PATH");
        std::string cert_path = cert_env ? cert_env : "test_cert.pem";
        std::string key_path = key_env ? key_env : "test_key.pem";
        
        
        if (!https_server->listenKTLS(port, cert_path, key_path)) {
            std::cerr << "Failed to start HTTPS server on port " << port << std::endl;
            return 1;
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

        // Serve any file under the docroot at /files/<path>
        // Using the new capture-aware API to avoid double regex execution!
        https_server->addRouteWithCaptures("GET", R"(^/files/(.+)$)", [docroot](const HttpRequest& req, HttpResponse& res, const std::smatch& match){
            if (match.size() >= 2) {
                std::string relpath = match[1].str();  // Extract capture group directly from router
                
                // Prevent path traversal: check for suspicious patterns first
                if (relpath.find("..") != std::string::npos || relpath.empty() || relpath[0] == '/') {
                    res.status_code = 403;
                    res.body = "403 Forbidden\n";
                    return;
                }
                
                // Map /files/<path> to <docroot>/files/<path>
                fs::path fp = fs::path(docroot) / "files" / relpath;
                
                // Normalize and ensure fp is inside docroot using lexically_relative
                fs::path norm = fs::weakly_canonical(fp);
                fs::path root = fs::weakly_canonical(docroot);
                
                // Use lexically_relative to check containment - if it starts with "..", it escaped
                auto rel = norm.lexically_relative(root);
                if (rel.empty() || rel.string().rfind("..", 0) == 0) {
                    res.status_code = 403;
                    res.body = "403 Forbidden\n";
                    return;
                }
                
                if (fs::exists(norm) && fs::is_regular_file(norm)) {
                    // Use sendFile for zero-copy transfer with MIME type detection
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

        std::cout << "Listening on https://0.0.0.0:" << port << " (KTLS enabled)\n";
        std::cout << "Press Ctrl+C to stop\n";
        // Run the server in a separate thread so we can handle signals cleanly
        std::thread server_thread([&]() {
            job_server->run();
        });
        // Wait for server thread to finish (sigwait thread triggers stop())
        if (server_thread.joinable()) {
            server_thread.join();
        }

        // Clean up explicitly after the server stops
        if (https_server) https_server.reset();
        if (job_server) job_server.reset();

        std::cout << "Shutdown complete" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
