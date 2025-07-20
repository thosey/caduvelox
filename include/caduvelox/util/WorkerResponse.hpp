#pragma once

#include "caduvelox/http/HttpTypes.hpp"
#include <memory>

namespace caduvelox {

/**
 * Message from worker thread to io_uring thread requesting a response be sent
 * 
 * Uses raw pointer pattern - connection validity checked via client_fd >= 0
 * If connection closes before response is processed, sendResponse will no-op safely
 */
struct WorkerResponse {
    int client_fd;                          // File descriptor to send response to
    HttpResponse response;                  // The response to send
    bool keep_alive;                        // Whether to keep connection alive
    class HttpConnectionJob* connection_job;  // Raw pointer to connection (validity checked in sendResponse)
    
    WorkerResponse() : client_fd(-1), keep_alive(false), connection_job(nullptr) {}
    
    WorkerResponse(int fd, HttpResponse resp, bool ka, class HttpConnectionJob* job)
        : client_fd(fd)
        , response(std::move(resp))
        , keep_alive(ka)
        , connection_job(job)
    {}
    
    // Movable
    WorkerResponse(WorkerResponse&&) = default;
    WorkerResponse& operator=(WorkerResponse&&) = default;
    
    // Non-copyable (HttpResponse may own large buffers)
    WorkerResponse(const WorkerResponse&) = delete;
    WorkerResponse& operator=(const WorkerResponse&) = delete;
};

} // namespace caduvelox
