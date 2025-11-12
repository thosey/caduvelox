#pragma once

#include "caduvelox/http/HttpTypes.hpp"
#include <memory>

namespace caduvelox {

/**
 * Message from worker thread to io_uring thread requesting a response be sent
 * 
 * Uses weak_ptr to safely handle connection lifetime across worker threads
 * If connection closes before response is processed, sendResponse will no-op safely
 */
struct WorkerResponse {
    int client_fd;                          // File descriptor to send response to
    HttpResponse response;                  // The response to send
    bool keep_alive;                        // Whether to keep connection alive
    std::weak_ptr<class HttpConnectionJob> connection_job;  // Safe pointer to connection
    
    WorkerResponse() : client_fd(-1), keep_alive(false) {}
    
    WorkerResponse(int fd, HttpResponse resp, bool ka, std::weak_ptr<class HttpConnectionJob> job)
        : client_fd(fd)
        , response(std::move(resp))
        , keep_alive(ka)
        , connection_job(std::move(job))
    {}
    
    // Movable
    WorkerResponse(WorkerResponse&&) = default;
    WorkerResponse& operator=(WorkerResponse&&) = default;
    
    // Non-copyable (HttpResponse may own large buffers)
    WorkerResponse(const WorkerResponse&) = delete;
    WorkerResponse& operator=(const WorkerResponse&) = delete;
};

} // namespace caduvelox
