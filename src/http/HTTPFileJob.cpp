#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/jobs/SpliceFileJob.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "LockFreeMemoryPool.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sstream>
#include <cstring>

// Define lock-free pool for HTTPFileJob at global scope  
// Medium pool since HTTP file serving is common but not as frequent as basic I/O
DEFINE_LOCKFREE_POOL(caduvelox::HTTPFileJob, 1000);

namespace caduvelox {

HTTPFileJob::HTTPFileJob(int client_fd, const std::string& file_path, uint64_t offset, uint64_t length, HttpResponse response)
    : state_(Opening)
    , client_fd_(client_fd)
    , file_path_(file_path)
    , offset_(offset)
    , length_(length)
    , response_(std::move(response))
    , file_fd_(-1)
    , file_size_(0)
    , header_size_(0) {
}

HTTPFileJob* HTTPFileJob::createFromPool(
    int client_fd,
    const std::string& file_path,
    HttpResponse response,
    CompletionCallback on_complete,
    ErrorCallback on_error) {
    
    return createFromPool(client_fd, file_path, 0, 0, std::move(response), std::move(on_complete), std::move(on_error));
}

HTTPFileJob* HTTPFileJob::createFromPool(
    int client_fd,
    const std::string& file_path,
    uint64_t offset,
    uint64_t length,
    HttpResponse response,
    CompletionCallback on_complete,
    ErrorCallback on_error) {
    
    HTTPFileJob* job = lfmemorypool::lockfree_pool_alloc_fast<HTTPFileJob>(client_fd, file_path, offset, length, std::move(response));
    if (job) {
        job->on_complete_ = std::move(on_complete);
        job->on_error_ = std::move(on_error);
    }
    return job;
}

void HTTPFileJob::start(Server& server) {
    Logger::getInstance().logMessage("HTTPFileJob: Starting file transfer fd=" + 
                                   std::to_string(client_fd_) + ", file=" + file_path_);
    
    openFile();
    
    if (file_fd_ < 0) {
        sendError(server, 404, "File not found");
        return;
    }
    
    startSendingHeaders(server);
}

void HTTPFileJob::openFile() {
    // Open file for reading
    file_fd_ = open(file_path_.c_str(), O_RDONLY);
    if (file_fd_ < 0) {
        Logger::getInstance().logError("HTTPFileJob: Failed to open file: " + file_path_);
        return;
    }
    
    // Get file size
    struct stat st;
    if (fstat(file_fd_, &st) < 0) {
        Logger::getInstance().logError("HTTPFileJob: Failed to stat file: " + file_path_);
        close(file_fd_);
        file_fd_ = -1;
        return;
    }
    
    file_size_ = st.st_size;
    
    // Validate range if specified
    if (offset_ > file_size_) {
        Logger::getInstance().logError("HTTPFileJob: Offset beyond file size");
        close(file_fd_);
        file_fd_ = -1;
        return;
    }
    
    // Adjust length if needed
    if (length_ == 0 || offset_ + length_ > file_size_) {
        length_ = file_size_ - offset_;
    }
    
    Logger::getInstance().logMessage("HTTPFileJob: File opened fd=" + std::to_string(file_fd_) + 
                                   ", size=" + std::to_string(file_size_) + 
                                   ", range=" + std::to_string(offset_) + "-" + std::to_string(offset_ + length_ - 1));
}

void HTTPFileJob::startSendingHeaders(Server& server) {
    state_ = SendingHeaders;
    
    // Set appropriate status code
    if (offset_ > 0 || length_ < file_size_) {
        response_.setStatus(206, "Partial Content");
        response_.headers["Content-Range"] = "bytes " + std::to_string(offset_) + "-" + 
                                           std::to_string(offset_ + length_ - 1) + "/" + std::to_string(file_size_);
    } else {
        response_.setStatus(200, "OK");
    }
    
    // Set Content-Length
    response_.headers["content-length"] = std::to_string(length_);
    
    // Try to determine Content-Type from file extension
    if (response_.headers.find("content-type") == response_.headers.end()) {
        std::string content_type = "application/octet-stream"; // default
        
        auto dot_pos = file_path_.find_last_of('.');
        if (dot_pos != std::string::npos) {
            std::string ext = file_path_.substr(dot_pos + 1);
            
            // Simple MIME type mapping
            if (ext == "html" || ext == "htm") content_type = "text/html";
            else if (ext == "css") content_type = "text/css";
            else if (ext == "js") content_type = "application/javascript";
            else if (ext == "json") content_type = "application/json";
            else if (ext == "png") content_type = "image/png";
            else if (ext == "jpg" || ext == "jpeg") content_type = "image/jpeg";
            else if (ext == "gif") content_type = "image/gif";
            else if (ext == "svg") content_type = "image/svg+xml";
            else if (ext == "txt") content_type = "text/plain";
        }
        
        response_.headers["content-type"] = content_type;
    }
    
    // Add CORS headers to allow cross-origin CSS/JS to load
    if (response_.headers.find("access-control-allow-origin") == response_.headers.end()) {
        response_.headers["access-control-allow-origin"] = "*";
    }
    
    // Build HTTP response headers
    std::ostringstream oss;
    oss << "HTTP/1.1 " << response_.status_code << " " << response_.status_text << "\r\n";
    
    for (const auto& [key, value] : response_.headers) {
        oss << key << ": " << value << "\r\n";
    }
    
    oss << "\r\n"; // End of headers
    
    std::string header_str = oss.str();
    header_size_ = header_str.size();
    
    // Create owned data for WriteJob
    header_data_ = std::make_unique<char[]>(header_size_);
    std::memcpy(header_data_.get(), header_str.data(), header_size_);
    
    // Create WriteJob for headers using pool allocation
    auto write_job = WriteJob::createFromPoolWithOwnedData(
        client_fd_,
        std::move(header_data_),
        header_size_,
        [this, &server](int fd, size_t bytes_written) {
            Logger::getInstance().logMessage("HTTPFileJob: Headers sent fd=" + std::to_string(fd) + 
                                           ", bytes=" + std::to_string(bytes_written));
            // Headers sent successfully, now send file content
            startSendingFile(server);
        },
        [this](int fd, int error) {
            Logger::getInstance().logError("HTTPFileJob: Header write error fd=" + std::to_string(fd) + 
                                         ", error=" + std::to_string(error));
            if (file_fd_ >= 0) {
                close(file_fd_);
                file_fd_ = -1;
            }
            if (on_error_) {
                on_error_(client_fd_, error);
            }
            lfmemorypool::lockfree_pool_free_fast<HTTPFileJob>(this); // Pool cleanup
        }
    );
    
    if (write_job) {
        // Register and start the write job
        Logger::getInstance().logMessage("HTTPFileJob: Registering WriteJob for headers");
        struct io_uring_sqe* sqe = server.registerJob(write_job);
        if (sqe) {
            write_job->prepareSqe(sqe);
            int submitted = server.submit();
            Logger::getInstance().logMessage("HTTPFileJob: WriteJob registered and submitted, count=" + std::to_string(submitted));
        } else {
            Logger::getInstance().logError("HTTPFileJob: Failed to get SQE for WriteJob");
            sendError(server, 500, "Internal Server Error");
        }
    } else {
        Logger::getInstance().logError("HTTPFileJob: Failed to allocate WriteJob from pool");
        sendError(server, 500, "Internal Server Error");
    }
}

void HTTPFileJob::startSendingFile(Server& server) {
    state_ = SendingFile;
    
    // Create SpliceFileJob for true zero-copy file transfer using splice(2) with pool allocation
    auto splice_job = SpliceFileJob::createFromPool(
        client_fd_,
        file_fd_,
        offset_,
        length_,
        [this](int fd, size_t bytes_transferred) {
            Logger::getInstance().logMessage("HTTPFileJob: Splice transfer complete fd=" + std::to_string(fd) + 
                                           ", bytes=" + std::to_string(bytes_transferred));
            close(file_fd_);
            file_fd_ = -1;
            
            if (on_complete_) {
                on_complete_(client_fd_, header_size_ + bytes_transferred);
            }
            lfmemorypool::lockfree_pool_free_fast<HTTPFileJob>(this); // Pool cleanup
        },
        [this](int fd, int error) {
            Logger::getInstance().logError("HTTPFileJob: Splice error fd=" + std::to_string(fd) + 
                                         ", error=" + std::to_string(error));
            if (file_fd_ >= 0) {
                close(file_fd_);
                file_fd_ = -1;
            }
            if (on_error_) {
                on_error_(client_fd_, error);
            }
            lfmemorypool::lockfree_pool_free_fast<HTTPFileJob>(this); // Pool cleanup
        }
    );
    
    if (splice_job) {
        // SpliceFileJob is also a composite job - start it directly
        Logger::getInstance().logMessage("HTTPFileJob: Starting SpliceFileJob");
        splice_job->start(server);
        Logger::getInstance().logMessage("HTTPFileJob: SpliceFileJob started");
    } else {
        Logger::getInstance().logError("HTTPFileJob: Failed to allocate SpliceFileJob from pool");
        if (file_fd_ >= 0) {
            close(file_fd_);
            file_fd_ = -1;
        }
        if (on_error_) {
            on_error_(client_fd_, ENOMEM);
        }
        lfmemorypool::lockfree_pool_free_fast<HTTPFileJob>(this);
    }
}

void HTTPFileJob::sendError(Server& server, int status_code, const std::string& message) {
    if (file_fd_ >= 0) {
        close(file_fd_);
        file_fd_ = -1;
    }
    
    // Create error response
    HttpResponse error_response;
    error_response.setStatus(status_code, message);
    error_response.headers["content-type"] = "text/plain";
    error_response.headers["content-length"] = std::to_string(message.length());
    
    std::ostringstream oss;
    oss << "HTTP/1.1 " << error_response.status_code << " " << error_response.status_text << "\r\n";
    for (const auto& [key, value] : error_response.headers) {
        oss << key << ": " << value << "\r\n";
    }
    oss << "\r\n" << message;
    
    std::string response_str = oss.str();
    auto response_data = std::make_unique<char[]>(response_str.size());
    std::memcpy(response_data.get(), response_str.data(), response_str.size());
    
    auto write_job = WriteJob::createFromPoolWithOwnedData(
        client_fd_,
        std::move(response_data),
        response_str.size(),
        [this](int fd, size_t bytes_written) {
            if (on_error_) {
                on_error_(client_fd_, 0); // Indicate error was handled
            }
            lfmemorypool::lockfree_pool_free_fast<HTTPFileJob>(this);
        },
        [this](int fd, int error) {
            if (on_error_) {
                on_error_(client_fd_, error);
            }
            lfmemorypool::lockfree_pool_free_fast<HTTPFileJob>(this);
        }
    );
    
    if (write_job) {
        // Register and start the write job  
        struct io_uring_sqe* sqe = server.registerJob(write_job);
        if (sqe) {
            write_job->prepareSqe(sqe);
            server.submit();
        } else {
            Logger::getInstance().logError("HTTPFileJob: Failed to get SQE for error WriteJob");
            // Failed to get SQE - free the write job and this job
            WriteJob::freePoolAllocated(write_job);
            if (on_error_) {
                on_error_(client_fd_, ENOMEM);
            }
            lfmemorypool::lockfree_pool_free_fast<HTTPFileJob>(this);
        }
    } else {
        Logger::getInstance().logError("HTTPFileJob: Failed to allocate error WriteJob from pool");
        if (on_error_) {
            on_error_(client_fd_, ENOMEM);
        }
        lfmemorypool::lockfree_pool_free_fast<HTTPFileJob>(this);
    }
}

} // namespace caduvelox
