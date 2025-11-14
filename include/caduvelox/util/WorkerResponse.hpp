#pragma once

#include "caduvelox/http/HttpTypes.hpp"

namespace caduvelox {

/**
 * Message from worker thread to io_uring thread requesting a response be sent
 * 
 * Uses raw pointer - io_uring thread will use tryAcquire/tryRelease for safe access
 */
struct WorkerResponse {
    int client_fd;                          // File descriptor to send response to
    HttpResponse response;                  // The response to send
    bool keep_alive;                        // Whether to keep connection alive
    class HttpConnectionJob* connection_job;  // Raw pointer - protected by atomic state
    
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
