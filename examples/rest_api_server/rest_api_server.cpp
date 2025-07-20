/**
 * REST API Server with Job-Based Architecture
 * 
 * Demonstrates a file management REST API using the job-based
 * architecture with zero-copy operations.
 * 
 * Usage: ./rest_api_server [data_dir] [port]
 * 
 * Features:
 * - Complete REST API for file management
 * - JSON responses with proper error handling
 * - Path traversal protection
 * - CORS support for browser testing
 * - Zero-copy operations with io_uring
 */

#include "caduvelox/Server.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/http/HttpTypes.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <signal.h>
#include <pthread.h>
#include <thread>
#include <chrono>

using namespace caduvelox;
namespace fs = std::filesystem;

static std::unique_ptr<Server> job_server;
static std::unique_ptr<HttpServer> http_server;

// Simple JSON helpers (no external dependencies)
std::string escape_json_string(const std::string& str) {
    std::string escaped;
    for (char c : str) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

std::string json_error(const std::string& message) {
    return "{\"error\":\"" + escape_json_string(message) + "\"}";
}

std::string parse_json_field(const std::string& json, const std::string& field) {
    std::regex field_regex("\"" + field + "\"\\s*:\\s*\"([^\"]*?)\"");
    std::smatch match;
    if (std::regex_search(json, match, field_regex)) {
        return match[1].str();
    }
    return "";
}

int main(int argc, char** argv) {
    try {
        // Usage: ./rest_api_server [data_dir] [port]
        std::string data_dir = argc > 1 ? argv[1] : "data";
        int port = argc > 2 ? std::stoi(argv[2]) : 8080;

        std::cout << "Starting REST API server (job-based)\n";
        std::cout << "Data directory: " << data_dir << "\n";
        std::cout << "Port: " << port << "\n";

        // Create data directory if it doesn't exist
        fs::create_directories(data_dir);

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

        // Initialize HTTP server
        http_server = std::make_unique<HttpServer>(*job_server);
        if (!http_server->listen(port)) {
            std::cerr << "Failed to start HTTP server on port " << port << std::endl;
            return 1;
        }

        // Helper to add CORS headers to responses
        auto add_cors_headers = [](HttpResponse& res) {
            res.setHeader("Access-Control-Allow-Origin", "*");
            res.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            res.setHeader("Access-Control-Allow-Headers", "Content-Type");
        };

        // Handle CORS preflight requests
        http_server->addRoute("OPTIONS", ".*", [add_cors_headers](const auto& req, auto& res) {
            (void)req;
            add_cors_headers(res);
            res.setStatus(204);
        });

        // GET /api/files - List all files in data directory
        http_server->addRoute("GET", R"(^/api/files$)", [data_dir, add_cors_headers](const auto& req, auto& res) {
            (void)req;
            add_cors_headers(res);
            try {
                std::stringstream files_json;
                files_json << "[";
                bool first = true;
                
                for (const auto& entry : fs::directory_iterator(data_dir)) {
                    if (entry.is_regular_file()) {
                        if (!first) files_json << ",";
                        files_json << "{";
                        files_json << "\"name\":\"" << escape_json_string(entry.path().filename().string()) << "\",";
                        files_json << "\"size\":" << entry.file_size() << ",";
                        files_json << "\"modified\":" << fs::last_write_time(entry).time_since_epoch().count();
                        files_json << "}";
                        first = false;
                    }
                }
                files_json << "]";
                
                res.json(files_json.str());
            } catch (const std::exception& e) {
                res.setStatus(500);
                res.json(json_error(e.what()));
            }
        });

        // GET /api/files/{filename} - Read a specific file
        http_server->addRoute("GET", R"(^/api/files/([^/]+)$)", [data_dir, add_cors_headers](const auto& req, auto& res) {
            add_cors_headers(res);
            static const std::regex file_re(R"(^/api/files/([^/]+)$)");
            std::smatch m;
            
            if (std::regex_match(req.path, m, file_re) && m.size() >= 2) {
                std::string filename = m[1].str();
                
                // Prevent path traversal: check for suspicious patterns
                if (filename.find("..") != std::string::npos || filename.empty() || filename.find('/') != std::string::npos) {
                    res.setStatus(403);
                    res.json(json_error("Access denied"));
                    return;
                }
                
                fs::path filepath = fs::path(data_dir) / filename;
                
                // Normalize and ensure filepath is inside data_dir using lexically_relative
                fs::path norm = fs::weakly_canonical(filepath);
                fs::path root = fs::weakly_canonical(data_dir);
                
                // Use lexically_relative to check containment - if it starts with "..", it escaped
                auto rel = norm.lexically_relative(root);
                if (rel.empty() || rel.string().rfind("..", 0) == 0) {
                    res.setStatus(403);
                    res.json(json_error("Access denied"));
                    return;
                }
                
                if (!fs::exists(norm) || !fs::is_regular_file(norm)) {
                    res.setStatus(404);
                    res.json(json_error("File not found"));
                    return;
                }
                
                try {
                    std::ifstream file(norm);
                    std::stringstream buffer;
                    buffer << file.rdbuf();
                    
                    std::stringstream response;
                    response << "{";
                    response << "\"filename\":\"" << escape_json_string(filename) << "\",";
                    response << "\"content\":\"" << escape_json_string(buffer.str()) << "\",";
                    response << "\"size\":" << fs::file_size(norm);
                    response << "}";
                    
                    res.json(response.str());
                } catch (const std::exception& e) {
                    res.setStatus(500);
                    res.json(json_error(e.what()));
                }
            } else {
                res.setStatus(400);
                res.json(json_error("Invalid filename"));
            }
        });

        // POST /api/files - Create a new file
        http_server->addRoute("POST", R"(^/api/files$)", [data_dir, add_cors_headers](const auto& req, auto& res) {
            add_cors_headers(res);
            try {
                std::string filename = parse_json_field(req.body, "filename");
                std::string content = parse_json_field(req.body, "content");
                
                if (filename.empty()) {
                    res.setStatus(400);
                    res.json(json_error("Missing filename"));
                    return;
                }
                
                // Basic filename validation
                if (filename.find("..") != std::string::npos || 
                    filename.find("/") != std::string::npos || filename.find("\\") != std::string::npos) {
                    res.setStatus(400);
                    res.json(json_error("Invalid filename"));
                    return;
                }
                
                fs::path filepath = fs::path(data_dir) / filename;
                
                if (fs::exists(filepath)) {
                    res.setStatus(409);
                    res.json(json_error("File already exists"));
                    return;
                }
                
                std::ofstream file(filepath);
                file << content;
                file.close();
                
                std::stringstream response;
                response << "{";
                response << "\"filename\":\"" << escape_json_string(filename) << "\",";
                response << "\"size\":" << content.length() << ",";
                response << "\"message\":\"File created successfully\"";
                response << "}";
                
                res.setStatus(201);
                res.json(response.str());
                
            } catch (const std::exception& e) {
                res.setStatus(500);
                res.json(json_error(e.what()));
            }
        });

        // PUT /api/files/{filename} - Update an existing file
        http_server->addRoute("PUT", R"(^/api/files/([^/]+)$)", [data_dir, add_cors_headers](const auto& req, auto& res) {
            add_cors_headers(res);
            static const std::regex file_re(R"(^/api/files/([^/]+)$)");
            std::smatch m;
            
            if (std::regex_match(req.path, m, file_re) && m.size() >= 2) {
                std::string filename = m[1].str();
                
                try {
                    std::string content = parse_json_field(req.body, "content");
                    
                    fs::path filepath = fs::path(data_dir) / filename;
                    
                    // Prevent path traversal
                    fs::path norm = fs::weakly_canonical(filepath);
                    fs::path root = fs::weakly_canonical(data_dir);
                    if (norm.string().rfind(root.string(), 0) != 0) {
                        res.setStatus(403);
                        res.json(json_error("Access denied"));
                        return;
                    }
                    
                    if (!fs::exists(norm)) {
                        res.setStatus(404);
                        res.json(json_error("File not found"));
                        return;
                    }
                    
                    std::ofstream file(norm);
                    file << content;
                    file.close();
                    
                    std::stringstream response;
                    response << "{";
                    response << "\"filename\":\"" << escape_json_string(filename) << "\",";
                    response << "\"size\":" << content.length() << ",";
                    response << "\"message\":\"File updated successfully\"";
                    response << "}";
                    
                    res.json(response.str());
                    
                } catch (const std::exception& e) {
                    res.setStatus(500);
                    res.json(json_error(e.what()));
                }
            } else {
                res.setStatus(400);
                res.json(json_error("Invalid filename"));
            }
        });

        // DELETE /api/files/{filename} - Delete a file
        http_server->addRoute("DELETE", R"(^/api/files/([^/]+)$)", [data_dir, add_cors_headers](const auto& req, auto& res) {
            (void)req;
            add_cors_headers(res);
            static const std::regex file_re(R"(^/api/files/([^/]+)$)");
            std::smatch m;
            
            if (std::regex_match(req.path, m, file_re) && m.size() >= 2) {
                std::string filename = m[1].str();
                fs::path filepath = fs::path(data_dir) / filename;
                
                // Prevent path traversal
                fs::path norm = fs::weakly_canonical(filepath);
                fs::path root = fs::weakly_canonical(data_dir);
                if (norm.string().rfind(root.string(), 0) != 0) {
                    res.setStatus(403);
                    res.json(json_error("Access denied"));
                    return;
                }
                
                if (!fs::exists(norm)) {
                    res.setStatus(404);
                    res.json(json_error("File not found"));
                    return;
                }
                
                try {
                    fs::remove(norm);
                    
                    std::stringstream response;
                    response << "{";
                    response << "\"filename\":\"" << escape_json_string(filename) << "\",";
                    response << "\"message\":\"File deleted successfully\"";
                    response << "}";
                    
                    res.json(response.str());
                    
                } catch (const std::exception& e) {
                    res.setStatus(500);
                    res.json(json_error(e.what()));
                }
            } else {
                res.setStatus(400);
                res.json(json_error("Invalid filename"));
            }
        });

        // Root endpoint with API documentation
        http_server->addRoute("GET", R"(^/$)", [add_cors_headers](const auto& req, auto& res) {
            (void)req;
            add_cors_headers(res);
            res.html(R"(<!DOCTYPE html>
<html>
<head>
    <title>Caduvelox REST API Server (Job-Based)</title>
    <style>
        body { font-family: system-ui, sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }
        code { background: #f4f4f4; padding: 2px 4px; border-radius: 3px; }
        pre { background: #f4f4f4; padding: 15px; border-radius: 5px; overflow-x: auto; }
        .endpoint { margin: 20px 0; padding: 15px; border-left: 4px solid #007acc; background: #f8f9fa; }
        .feature { margin: 10px 0; padding: 10px; border-left: 4px solid #28a745; background: #f8f9fa; }
    </style>
</head>
<body>
    <h1>Caduvelox REST API Server</h1>
    <p>A high-performance file management REST API built with Caduvelox's job-based architecture.</p>
    
    <h2>Architecture Features</h2>
    <div class="feature">
        <strong>Job-Based Architecture:</strong> Pure io_uring-driven request processing
    </div>
    <div class="feature">
        <strong>Zero-Copy Operations:</strong> Efficient buffer management with ring buffers
    </div>
    <div class="feature">
        <strong>High Performance:</strong> Lock-free data structures and minimal memory allocation
    </div>
    
    <h2>API Endpoints</h2>
    
    <div class="endpoint">
        <h3>GET /api/files</h3>
        <p>List all files in the data directory</p>
        <pre>curl http://localhost:8080/api/files</pre>
    </div>
    
    <div class="endpoint">
        <h3>GET /api/files/{filename}</h3>
        <p>Read the content of a specific file</p>
        <pre>curl http://localhost:8080/api/files/example.txt</pre>
    </div>
    
    <div class="endpoint">
        <h3>POST /api/files</h3>
        <p>Create a new file</p>
        <pre>curl -X POST -H "Content-Type: application/json" \
     -d '{"filename":"test.txt","content":"Hello World!"}' \
     http://localhost:8080/api/files</pre>
    </div>
    
    <div class="endpoint">
        <h3>PUT /api/files/{filename}</h3>
        <p>Update an existing file</p>
        <pre>curl -X PUT -H "Content-Type: application/json" \
     -d '{"content":"Updated content"}' \
     http://localhost:8080/api/files/test.txt</pre>
    </div>
    
    <div class="endpoint">
        <h3>DELETE /api/files/{filename}</h3>
        <p>Delete a file</p>
        <pre>curl -X DELETE http://localhost:8080/api/files/test.txt</pre>
    </div>
    
    <h2>Test the API</h2>
    <p>Try creating a test file:</p>
    <pre>curl -X POST -H "Content-Type: application/json" \
     -d '{"filename":"hello.txt","content":"Hello from Caduvelox!"}' \
     http://localhost:8080/api/files</pre>
</body>
</html>)");
        });

        std::cout << "Listening on http://0.0.0.0:" << port << "\n";
        std::cout << "API documentation: http://localhost:" << port << "/\n";
        std::cout << "Data directory: " << fs::absolute(data_dir) << "\n";
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
        if (http_server) http_server.reset();
        if (job_server) job_server.reset();

        std::cout << "Shutdown complete" << std::endl;
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
