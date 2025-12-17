#include "caduvelox/jobs/SpliceFileJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <liburing.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <errno.h>

// Pool capacity specialization for SpliceFileJob
template<>
constexpr size_t caduvelox::PoolManager::getPoolCapacity<caduvelox::SpliceFileJob>() {
    return 1000; // Zero-copy file transfer operations
}

namespace {
    void cleanupSpliceFileJob(caduvelox::IoJob* job) {
        caduvelox::PoolManager::deallocate(static_cast<caduvelox::SpliceFileJob*>(job));
    }
}

namespace caduvelox {

SpliceFileJob::SpliceFileJob(int client_fd, int file_fd, uint64_t offset, uint64_t length)
    : state_(CreatingPipe)
    , client_fd_(client_fd)
    , file_fd_(file_fd)
    , offset_(offset)
    , remaining_(length)
    , total_transferred_(0)
    , bytes_in_pipe_(0)
    , pending_operations_(0)
    , current_chunk_size_(0) {
    
    pipe_fds_[0] = -1;
    pipe_fds_[1] = -1;
}

SpliceFileJob::~SpliceFileJob() {
    cleanup();
}

SpliceFileJob* SpliceFileJob::createFromPool(
    int client_fd, 
    int file_fd, 
    uint64_t offset, 
    uint64_t length,
    CompletionCallback on_complete,
    ErrorCallback on_error) {
    
    SpliceFileJob* job = PoolManager::allocate<SpliceFileJob>(client_fd, file_fd, offset, length);
    if (job) {
        job->is_pool_allocated_ = true;
        job->on_complete_ = std::move(on_complete);
        job->on_error_ = std::move(on_error);
    }
    return job;
}

void SpliceFileJob::prepareSqe(struct io_uring_sqe* sqe) {
    switch (state_) {
        case SplicingFileToPipe: {
            // Determine chunk size for this splice operation
            size_t chunk_size = SPLICE_CHUNK_SIZE;
            if (remaining_ > 0 && remaining_ < chunk_size) {
                chunk_size = remaining_;
            }
            
            // splice(file_fd → pipe[1])
            io_uring_prep_splice(sqe, 
                                file_fd_, offset_,    // source: file at offset
                                pipe_fds_[1], -1,    // dest: pipe write end (offset ignored for pipes)
                                chunk_size,
                                0);  // no special flags
            break;
        }
        
        case SplicingPipeToSocket: {
            // splice(pipe[0] → socket_fd)
            io_uring_prep_splice(sqe,
                                pipe_fds_[0], -1,    // source: pipe read end
                                client_fd_, -1,      // dest: socket (offset ignored)
                                bytes_in_pipe_,      // transfer all bytes currently in pipe
                                0);  // no special flags
            break;
        }
        
        default:
            // CreatingPipe state doesn't use SQE (we create pipe synchronously)
            break;
    }
}

std::optional<IoJob::CleanupCallback> SpliceFileJob::handleCompletion(Server& server, struct io_uring_cqe* cqe) {
    ssize_t result = cqe->res;
    
    // Handle linked operations
    if (pending_operations_ > 0) {
        pending_operations_--;
        
        if (result < 0) {
            // Handle EAGAIN/EWOULDBLOCK - socket buffer full, retry
            if (-result == EAGAIN || -result == EWOULDBLOCK) {
                Logger::getInstance().logMessage("SpliceFileJob: Got EAGAIN/EWOULDBLOCK, resubmitting operation");
                pending_operations_++;  // Restore counter
                // Resubmit the same operation
                resubmit(server);
                return std::nullopt; // Continue operation
            }
            
            Logger::getInstance().logError("SpliceFileJob: linked splice operation failed fd=" + 
                                         std::to_string(client_fd_) + ", error=" + std::to_string(-result));
            if (on_error_) {
                on_error_(client_fd_, -result);
            }
            // Return cleanup function if pool allocated
            if (is_pool_allocated_) {
                return cleanupSpliceFileJob;
            }
            return std::nullopt; // Complete on error
        }
        
        if (state_ == SplicingFileToPipe) {
            // This is the file→pipe completion
            Logger::getInstance().logMessage("SpliceFileJob: File->Pipe linked splice: " + std::to_string(result) + " bytes");
            
            offset_ += result;
            bytes_in_pipe_ += result;
            current_chunk_size_ = result;
            
            if (remaining_ > 0) {
                remaining_ -= std::min(remaining_, static_cast<uint64_t>(result));
            }
            
            // Switch state for next completion (pipe→socket)
            state_ = SplicingPipeToSocket;
            
            // Still have one more operation pending (pipe→socket)
            return std::nullopt; // Continue operation
            
        } else if (state_ == SplicingPipeToSocket) {
            // This is the pipe→socket completion
            Logger::getInstance().logMessage("SpliceFileJob: Pipe->Socket linked splice: " + std::to_string(result) + " bytes, total=" + 
                                           std::to_string(total_transferred_ + result) + ", bytes_in_pipe before: " + std::to_string(bytes_in_pipe_));
            
            total_transferred_ += result;
            bytes_in_pipe_ -= result;
            
            // CRITICAL: Check if pipe still has bytes (partial write)
            if (bytes_in_pipe_ > 0) {
                Logger::getInstance().logMessage("SpliceFileJob: Partial pipe->socket write, " + 
                                               std::to_string(bytes_in_pipe_) + " bytes still in pipe, draining...");
                // Must drain the pipe before starting next file->pipe operation
                drainPipeToSocket(server);
                return std::nullopt; // Continue draining
            }
            
            // Pipe is empty, both linked operations completed
            if (remaining_ > 0) {
                // Start next linked splice for remaining data
                startLinkedSplice(server);
                return std::nullopt; // Continue with next chunk
            } else {
                // Transfer complete
                Logger::getInstance().logMessage("SpliceFileJob: Transfer complete fd=" + 
                                               std::to_string(client_fd_) + ", total=" + std::to_string(total_transferred_));
                if (on_complete_) {
                    on_complete_(client_fd_, total_transferred_);
                }
                // Return cleanup function if pool allocated
                if (is_pool_allocated_) {
                    return cleanupSpliceFileJob;
                }
                return std::nullopt; // Complete transfer
            }
        }
        
        return std::nullopt; // Continue operation
    }
    
    // This code should not be reached if using linked operations
    Logger::getInstance().logError("SpliceFileJob: Unexpected non-linked completion");
    return std::nullopt; // Complete on error
}

void SpliceFileJob::start(Server& server) {
    Logger::getInstance().logMessage("SpliceFileJob: Starting splice transfer fd=" + 
                                   std::to_string(client_fd_) + " from file_fd=" + std::to_string(file_fd_));
    
    // Create the pipe first
    createPipe();
    
    if (pipe_fds_[0] < 0 || pipe_fds_[1] < 0) {
        Logger::getInstance().logError("SpliceFileJob: Failed to create pipe");
        if (on_error_) {
            on_error_(client_fd_, ENOMEM);
        }
        return;
    }
    
    // Start the first linked splice operation (file → pipe → socket)
    startLinkedSplice(server);
}

void SpliceFileJob::createPipe() {
    if (pipe(pipe_fds_) < 0) {
        Logger::getInstance().logError("SpliceFileJob: pipe() failed");
        return;
    }
    
    // Set pipes to non-blocking mode for better io_uring integration
    fcntl(pipe_fds_[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fds_[1], F_SETFL, O_NONBLOCK);
    
    Logger::getInstance().logMessage("SpliceFileJob: Created pipe [" + 
                                   std::to_string(pipe_fds_[0]) + ", " + std::to_string(pipe_fds_[1]) + "]");
}

void SpliceFileJob::startLinkedSplice(Server& server) {
    // Determine chunk size for this splice operation
    size_t chunk_size = SPLICE_CHUNK_SIZE;
    if (remaining_ > 0 && remaining_ < chunk_size) {
        chunk_size = remaining_;
    }
    
    if (chunk_size == 0) {
        // No more data to transfer
        Logger::getInstance().logMessage("SpliceFileJob: Transfer complete fd=" + 
                                       std::to_string(client_fd_) + ", total=" + std::to_string(total_transferred_));
        if (on_complete_) {
            on_complete_(client_fd_, total_transferred_);
        }
        return;
    }
    
    Logger::getInstance().logMessage("SpliceFileJob: Starting linked splice - chunk_size=" + 
                                   std::to_string(chunk_size) + ", offset=" + std::to_string(offset_));
    
    // Get SQE for file→pipe splice (stage 1)
    struct io_uring_sqe* sqe1 = server.registerJob(this);
    if (!sqe1) {
        if (on_error_) {
            on_error_(client_fd_, -ENOMEM);
        }
        return;
    }
    
    // Set up file→pipe splice with linking
    io_uring_prep_splice(sqe1, 
                        file_fd_, offset_,           // source: file at offset
                        pipe_fds_[1], -1,           // dest: pipe write end
                        chunk_size,
                        0);  // no special flags
    sqe1->flags |= IOSQE_IO_LINK;  // Link to next operation
    
    // Get SQE for pipe→socket splice (stage 2) 
    struct io_uring_sqe* sqe2 = server.registerJob(this);
    if (!sqe2) {
        if (on_error_) {
            on_error_(client_fd_, -ENOMEM);
        }
        return;
    }
    
    // Set up pipe→socket splice (linked)
    io_uring_prep_splice(sqe2,
                        pipe_fds_[0], -1,           // source: pipe read end
                        client_fd_, -1,             // dest: socket
                        chunk_size,                 // transfer same amount
                        0);  // no special flags
    
    // Mark that we're expecting two completions
    state_ = SplicingFileToPipe;  // We'll track state through completions
    pending_operations_ = 2;      // Expect 2 completions
    current_chunk_size_ = chunk_size;
    
    // Submit both linked operations
    server.submit();
}

void SpliceFileJob::drainPipeToSocket(Server& server) {
    // Drain remaining bytes from pipe to socket (after partial write)
    // We know bytes_in_pipe_ > 0, so submit a pipe→socket splice for those bytes
    
    Logger::getInstance().logMessage("SpliceFileJob: Draining " + std::to_string(bytes_in_pipe_) + 
                                   " bytes from pipe to socket");
    
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (!sqe) {
        Logger::getInstance().logError("SpliceFileJob: Failed to get SQE for drain operation");
        if (on_error_) {
            on_error_(client_fd_, -ENOMEM);
        }
        return;
    }
    
    // Set up pipe→socket splice for remaining bytes
    io_uring_prep_splice(sqe,
                        pipe_fds_[0], -1,           // source: pipe read end
                        client_fd_, -1,             // dest: socket
                        bytes_in_pipe_,             // drain all remaining bytes
                        0);                         // no special flags
    
    // Stay in SplicingPipeToSocket state
    state_ = SplicingPipeToSocket;
    pending_operations_ = 1;  // Expect 1 completion for the drain
    
    server.submit();
}

void SpliceFileJob::resubmit(Server& server) {
    // Resubmit the current operation
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (sqe) {
        prepareSqe(sqe);
        server.submit();
    } else {
        if (on_error_) {
            on_error_(client_fd_, -ENOMEM);
        }
    }
}

void SpliceFileJob::cleanup() {
    if (pipe_fds_[0] >= 0) {
        close(pipe_fds_[0]);
        pipe_fds_[0] = -1;
    }
    if (pipe_fds_[1] >= 0) {
        close(pipe_fds_[1]);
        pipe_fds_[1] = -1;
    }
}

} // namespace caduvelox
