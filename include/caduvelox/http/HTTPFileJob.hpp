#pragma once

#include "caduvelox/http/HttpTypes.hpp"
#include "caduvelox/http/RangeRequest.hpp"
#include <cstdint>
#include <functional>
#include <optional>
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
     * Serve the range the client asked for, instead of the whole file.
     *
     * Takes the range in the form the client wrote it, not an offset: a suffix
     * range ("the last 500 bytes") has no offset until the file has been
     * stat()ed, and openFile() is the only place that happens. Resolving there
     * means the same fstat(2) decides the Content-Range and the bytes spliced.
     *
     * A range that turns out not to be satisfiable becomes a 416 carrying the
     * real length. Must be called before start().
     */
    void requestRange(const http::ByteRangeSpec& spec);

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
    // The range as the client wrote it, when one was asked for. Resolved
    // against the real file size in openFile(); see requestRange().
    std::optional<http::ByteRangeSpec> range_spec_;
    // True once this response is known to be a partial one, i.e. 206 with a
    // Content-Range rather than 200 with the whole file.
    bool partial_ = false;
    HttpResponse response_;
    
    int file_fd_;
    // Set when openFile() refused because the requested range starts past the end
    // of the file: the value is the file's size, which a 416 reports back so a
    // resuming client learns the real length. Distinguishes that refusal from a
    // file that is simply not there, which is otherwise the same (file_fd_ < 0).
    std::optional<uint64_t> range_not_satisfiable_size_;
    uint64_t file_size_;
    // Last modification time of the file being served, from the same fstat(2)
    // that gave file_size_. Sent as Last-Modified so a client resuming an
    // interrupted range transfer has something to validate against.
    int64_t file_mtime_ = 0;
    std::unique_ptr<char[]> header_data_;
    size_t header_size_;
    
    CompletionCallback on_complete_;
    ErrorCallback on_error_;
};

} // namespace caduvelox