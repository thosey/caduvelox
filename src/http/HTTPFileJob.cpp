#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/jobs/SpliceFileJob.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sstream>
#include <cstring>

// Default pool capacity for HTTPFileJob (overridable at runtime via ServerConfig).
template<>
size_t caduvelox::PoolCapacityConfig<caduvelox::HTTPFileJob>::capacity = 1000;

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
    
    HTTPFileJob* job = PoolManager::allocate<HTTPFileJob>(client_fd, file_path, offset, length, std::move(response));
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
    // open(2) is not a "give me this file" call; it is a "do whatever this inode
    // says" call, and for anything that is not a regular file that means
    // something other than reading bytes.
    //
    // O_NONBLOCK is the load-bearing flag. Opening a FIFO for reading blocks
    // *inside open()* until a writer arrives -- on the ring thread, which is not
    // waiting on io_uring at that point and cannot service any of the other
    // connections that core owns. A route that maps any part of a request onto a
    // path can freeze a whole core that way. With the flag the open returns
    // immediately, and the S_ISREG check below is what then refuses it.
    //
    // O_CLOEXEC keeps a served file out of anything this process later execs.
    //
    // Deliberately *not* O_NOFOLLOW: it refuses a symlink only as the final path
    // component, so it does not stop traversal through a symlinked directory,
    // while it does break the ordinary case of a symlinked file inside a
    // document root. Confining paths to a root is the caller's decision -- see
    // the weakly_canonical() check in examples/static_https_server.
    file_fd_ = open(file_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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

    // Only regular files can be served, and the check has to happen here --
    // before startSendingHeaders() -- because by the time splice(2) fails there
    // is no way left to report it. A directory opens happily and reports a
    // plausible st_size, so without this the client gets a 200 and a
    // Content-Length copied off the directory inode, and then the connection
    // dies mid-body when splice returns EINVAL. Devices, sockets and FIFOs are
    // the same shape of problem.
    //
    // Reported as 404 (by start(), which sees only the failed open) rather than
    // 403, so the response does not confirm what is on disk.
    if (!S_ISREG(st.st_mode)) {
        Logger::getInstance().logError("HTTPFileJob: Not a regular file, refusing to serve: " + file_path_);
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
    
    // An empty file has no last byte; "offset + length - 1" would underflow.
    const std::string range = length_ == 0
        ? "empty"
        : std::to_string(offset_) + "-" + std::to_string(offset_ + length_ - 1);
    Logger::getInstance().logMessage("HTTPFileJob: File opened fd=" + std::to_string(file_fd_) +
                                   ", size=" + std::to_string(file_size_) +
                                   ", range=" + range);
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
            PoolManager::deallocate<HTTPFileJob>(this); // Pool cleanup
        }
    );
    
    if (!write_job) {
        Logger::getInstance().logError("HTTPFileJob: Failed to allocate WriteJob from pool");
        sendError(server, 500, "Internal Server Error");
        return;
    }

    // Hand the job to start() rather than driving the ring here. This used to be
    // an open-coded copy of WriteJob::submitWrite() that asked for one SQE and,
    // if the ring had none to give, dropped the job on the floor -- nothing was
    // queued, so no completion ever came to free it and the pool slot was lost
    // for good.
    //
    // It also gave up where submitWrite() retries. A full submission queue is
    // transient; flushing it frees every slot at once, so the request now
    // survives a momentary shortage instead of failing.
    //
    // start() owns write_job from here, and on the failing path it runs the
    // error callback above -- which closes file_fd_ and deallocates this job.
    // Neither pointer may be touched afterwards. Nothing is attempted for a 500
    // in that case: producing one needs the very SQE that could not be had, so
    // sendError() would allocate a second WriteJob only to land in its own
    // failure branch and tear down exactly the same way.
    Logger::getInstance().logMessage("HTTPFileJob: Submitting WriteJob for headers");
    write_job->start(server);
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
            PoolManager::deallocate<HTTPFileJob>(this); // Pool cleanup
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
            PoolManager::deallocate<HTTPFileJob>(this); // Pool cleanup
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
        PoolManager::deallocate<HTTPFileJob>(this);
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

    // Carry the caller's connection disposition across. response_ is built by
    // the connection, which sets "connection: close" exactly when it intends to
    // close after this response; this reply is a fresh HttpResponse and would
    // otherwise drop it, leaving an HTTP/1.1 client to assume the connection
    // persists right up until it does not.
    if (auto it = response_.headers.find("connection"); it != response_.headers.end()) {
        error_response.headers["connection"] = it->second;
    }
    
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
        // An error response that reached the socket intact is a delivered
        // response, so it completes rather than fails.
        //
        // This used to report through on_error_ with an error code of 0, meaning
        // "handled" -- but nothing downstream reads the code, and the
        // connection's error callback closes the connection whatever it says. So
        // every 404 tore down a connection whose response had gone out in full.
        // A missing file is an ordinary answer to an ordinary request; the
        // client that paid for it was the one doing what HTTP/1.1 asks, reusing
        // a connection across resources that do not all exist.
        //
        // The mid-transfer failures still report as errors, and must: by then
        // headers and a Content-Length are on the wire, and the connection is
        // the only thing left that can signal the truncation.
        [this](int fd, size_t bytes_written) {
            if (on_complete_) {
                on_complete_(client_fd_, bytes_written);
            }
            PoolManager::deallocate<HTTPFileJob>(this);
        },
        [this](int fd, int error) {
            if (on_error_) {
                on_error_(client_fd_, error);
            }
            PoolManager::deallocate<HTTPFileJob>(this);
        }
    );
    
    if (!write_job) {
        Logger::getInstance().logError("HTTPFileJob: Failed to allocate error WriteJob from pool");
        if (on_error_) {
            on_error_(client_fd_, ENOMEM);
        }
        PoolManager::deallocate<HTTPFileJob>(this);
        return;
    }

    // As in startSendingHeaders(): start() owns the job, and its error callback
    // above already performs the teardown the open-coded failure branch used to
    // duplicate. This job may be gone once start() returns.
    write_job->start(server);
}

} // namespace caduvelox
