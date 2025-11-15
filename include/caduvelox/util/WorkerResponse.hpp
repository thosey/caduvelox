#pragma once

#include "caduvelox/http/HttpTypes.hpp"
#include <memory>

namespace caduvelox {

// Forward declare
class HttpConnectionJob;

/**
 * Message from worker thread to io_uring thread requesting a response be sent
 * 
 * Uses shared_ptr to keep HttpConnectionJob alive during queue transition
 * The shared_ptr uses a custom pool deleter, so no malloc overhead!
 */
struct WorkerResponse {
    int client_fd;                          // File descriptor to send response to
    HttpResponse response;                  // The response to send
    bool keep_alive;                        // Whether to keep connection alive
    std::shared_ptr<HttpConnectionJob> connection_job;  // Keeps job alive via refcount
    
    WorkerResponse() : client_fd(-1), keep_alive(false), connection_job(nullptr) {}
    
    WorkerResponse(int fd, HttpResponse resp, bool ka, std::shared_ptr<HttpConnectionJob> job)
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
