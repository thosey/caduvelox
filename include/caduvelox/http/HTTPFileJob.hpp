#pragma once

#include "caduvelox/http/HttpTypes.hpp"
#include <functional>
#include <memory>
#include <string>

namespace caduvelox {

// Forward declaration
class Server;

/**
 * Composite job for serving HTTP files with proper headers.
 * 
 * This orchestrates the complete HTTP file response:
 * 1. WriteJob: Send HTTP response headers
 * 2. SpliceFileJob: True zero-copy file transfer via splice(2)
 * 
 * Not an IoJob itself - just a helper that coordinates real io_uring jobs.
 * All HTTPFileJobs are pool-allocated for performance.
 */
class HTTPFileJob {
public:
    using CompletionCallback = std::function<void(int client_fd, size_t bytes_sent)>;
    using ErrorCallback = std::function<void(int client_fd, int error)>;

    /**
     * Create HTTP file job using lock-free pool allocation.
     * @param client_fd Socket to send response to
     * @param file_path Path to file to serve
     * @param response HTTP response object (headers will be added automatically)
     * @param on_complete Callback when file is fully sent
     * @param on_error Callback on file or transfer errors
     * @return Pointer to pool-allocated HTTPFileJob, or nullptr if pool exhausted
     */
    static HTTPFileJob* createFromPool(
        int client_fd,
        const std::string& file_path,
        HttpResponse response = HttpResponse{},
        CompletionCallback on_complete = nullptr,
        ErrorCallback on_error = nullptr
    );

    /**
     * Create HTTP file job for range request using lock-free pool allocation.
     * @param client_fd Socket to send response to
     * @param file_path Path to file to serve
     * @param offset Starting byte offset
     * @param length Number of bytes to send (0 = to end)
     * @param response HTTP response object (headers will be added automatically)
     * @param on_complete Callback when file is fully sent
     * @param on_error Callback on file or transfer errors
     * @return Pointer to pool-allocated HTTPFileJob, or nullptr if pool exhausted
     */
    static HTTPFileJob* createFromPool(
        int client_fd,
        const std::string& file_path,
        uint64_t offset,
        uint64_t length,
        HttpResponse response = HttpResponse{},
        CompletionCallback on_complete = nullptr,
        ErrorCallback on_error = nullptr
    );

    /**
     * Start the HTTP file transfer
     */
    void start(Server& server);

    // Public constructor for pool allocation
    HTTPFileJob(int client_fd, const std::string& file_path, uint64_t offset, uint64_t length, HttpResponse response);

private:
    enum State {
        Opening,     // Opening file to get size/check existence
        SendingHeaders,  // Sending HTTP headers via WriteJob
        SendingFile      // Sending file content via SendFileJob
    };

    void openFile();
    void startSendingHeaders(Server& server);
    void startSendingFile(Server& server);
    void sendError(Server& server, int status_code, const std::string& message);

    State state_;
    int client_fd_;
    std::string file_path_;
    uint64_t offset_;
    uint64_t length_;
    HttpResponse response_;
    
    int file_fd_;
    uint64_t file_size_;
    std::unique_ptr<char[]> header_data_;
    size_t header_size_;
    
    CompletionCallback on_complete_;
    ErrorCallback on_error_;
};

} // namespace caduvelox