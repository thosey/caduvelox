#include "caduvelox/jobs/SpliceFileJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <liburing.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

// Default pool capacity for SpliceFileJob (overridable at runtime via ServerConfig).
template<>
size_t caduvelox::PoolCapacityConfig<caduvelox::SpliceFileJob>::capacity = 1000;

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
    , current_chunk_size_(0)
    , error_pending_(false)
    , retry_pending_(false)
    , deferred_error_(0)
    , eof_reached_(false) {
    
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
        job->on_complete_ = std::move(on_complete);
        job->on_error_ = std::move(on_error);
    }
    return job;
}

void SpliceFileJob::prepareSqe(struct io_uring_sqe* sqe) {
    switch (state_) {
        case SplicingFileToPipe: {
            // Determine chunk size for this splice operation. remaining_ is an
            // exact byte count, so clamp unconditionally — see startLinkedSplice().
            size_t chunk_size = static_cast<size_t>(
                std::min<uint64_t>(SPLICE_CHUNK_SIZE, remaining_));

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
            if (-result == ECANCELED) {
                // The kernel completes the linked partner with -ECANCELED when its
                // predecessor fails. The predecessor's completion already recorded
                // what to do (retry_pending_ / deferred_error_); act only once the
                // whole pair has drained — freeing this job earlier would leave a
                // kernel op pointing at freed memory.
                if (pending_operations_ > 0) {
                    return std::nullopt;
                }
                if (retry_pending_) {
                    retry_pending_ = false;
                    // Restart the full linked pair; if nothing could be
                    // submitted, the error callback has fired — free the job.
                    if (startLinkedSplice(server)) {
                        return std::nullopt;
                    }
                    return cleanupSpliceFileJob;
                }
                int err = deferred_error_ != 0 ? deferred_error_ : ECANCELED;
                deferred_error_ = 0;
                Logger::getInstance().logError("SpliceFileJob: linked splice pair failed fd=" +
                                             std::to_string(client_fd_) + ", error=" + std::to_string(err));
                if (on_error_) {
                    on_error_(client_fd_, err);
                }
                return cleanupSpliceFileJob;
            }

            // Handle EAGAIN/EWOULDBLOCK - socket buffer full, retry
            if (-result == EAGAIN || -result == EWOULDBLOCK) {
                if (pending_operations_ > 0) {
                    // First leg of a linked pair: its partner completes with
                    // -ECANCELED next. Defer the retry until the pair drains,
                    // then restart the full pair (resubmitting just this leg
                    // would desync the state machine from what's in flight).
                    Logger::getInstance().logMessage("SpliceFileJob: EAGAIN on linked leg, retrying pair after drain");
                    retry_pending_ = true;
                    return std::nullopt;
                }
                if (error_pending_) {
                    // Half-submitted pair (sqe2 allocation failed): don't retry a
                    // lone leg; report the original submission failure.
                    if (on_error_) {
                        on_error_(client_fd_, ENOMEM);
                    }
                    return cleanupSpliceFileJob;
                }
                Logger::getInstance().logMessage("SpliceFileJob: Got EAGAIN/EWOULDBLOCK, resubmitting operation");
                pending_operations_++;  // Restore counter
                // Resubmit the same standalone operation (pipe→socket leg or drain);
                // if resubmission failed, the error callback has fired — free the job.
                if (resubmit(server)) {
                    return std::nullopt; // Continue operation
                }
                return cleanupSpliceFileJob;
            }

            // Real error. If the linked partner is still in flight, its
            // -ECANCELED completion must drain before this job can be freed.
            if (pending_operations_ > 0) {
                deferred_error_ = -result;
                return std::nullopt;
            }
            Logger::getInstance().logError("SpliceFileJob: linked splice operation failed fd=" +
                                         std::to_string(client_fd_) + ", error=" + std::to_string(-result));
            if (on_error_) {
                on_error_(client_fd_, -result);
            }
            return cleanupSpliceFileJob;
        }

        // sqe2 allocation failed in startLinkedSplice; sqe1 just completed.
        // The overall transfer failed — call the error callback and clean up.
        if (error_pending_ && pending_operations_ == 0) {
            if (on_error_) {
                on_error_(client_fd_, ENOMEM);
            }
            return cleanupSpliceFileJob;
        }

        if (state_ == SplicingFileToPipe) {
            if (result == 0) {
                // EOF on the source file while bytes are still owed: the file was
                // truncated between the caller's fstat() (which fixed
                // Content-Length) and this splice. There is nothing left to send,
                // so the transfer must end here — retrying would splice at EOF
                // forever.
                //
                // The linked pipe→socket partner is still outstanding and the job
                // must stay alive until it drains. Two things can happen to it:
                //
                //  1. A short result severs an IOSQE_IO_LINK chain, and 0 is short
                //     of the requested chunk, so the kernel normally cancels the
                //     partner with -ECANCELED. deferred_error_ makes that branch
                //     report the truncation instead of a bare ECANCELED.
                //  2. If the partner does run, it would splice from an empty pipe
                //     whose write end we still hold open — an operation that never
                //     completes, hanging the connection and, worse, hanging
                //     Server::drainCompletions() on in_flight_ so run() never
                //     returns at shutdown. Closing the write end turns that splice
                //     into a clean EOF, so the pair drains either way.
                Logger::getInstance().logError("SpliceFileJob: EOF on file fd=" +
                                             std::to_string(file_fd_) + " with " +
                                             std::to_string(remaining_) + " bytes still expected");
                eof_reached_ = true;
                deferred_error_ = EIO;
                closePipeWriteEnd();
                // Route case 2's completion to the pipe→socket branch, which ends
                // the transfer. Do NOT free the job here: the kernel still holds
                // this pointer as the partner's user_data.
                state_ = SplicingPipeToSocket;
                return std::nullopt;
            }

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
                // Must drain the pipe before starting next file->pipe operation;
                // if nothing could be submitted, the error callback has fired.
                if (drainPipeToSocket(server)) {
                    return std::nullopt; // Continue draining
                }
                return cleanupSpliceFileJob;
            }

            // Pipe is empty, both linked operations completed
            if (eof_reached_ && remaining_ > 0) {
                // The file ended before we could deliver everything Content-Length
                // promised, so the response body is short and the stream is
                // unrecoverable. Report an error rather than completing: the owner
                // closes the connection instead of reusing it for another request.
                Logger::getInstance().logError("SpliceFileJob: file truncated during transfer fd=" +
                                             std::to_string(client_fd_) + ", " +
                                             std::to_string(remaining_) + " bytes undelivered");
                const int err = deferred_error_ != 0 ? deferred_error_ : EIO;
                deferred_error_ = 0;
                if (on_error_) {
                    on_error_(client_fd_, err);
                }
                return cleanupSpliceFileJob;
            }

            if (remaining_ > 0) {
                // Start next linked splice for remaining data; if nothing could
                // be submitted, the completion/error callback has fired.
                if (startLinkedSplice(server)) {
                    return std::nullopt; // Continue with next chunk
                }
                return cleanupSpliceFileJob;
            } else {
                // Transfer complete
                Logger::getInstance().logMessage("SpliceFileJob: Transfer complete fd=" + 
                                               std::to_string(client_fd_) + ", total=" + std::to_string(total_transferred_));
                if (on_complete_) {
                    on_complete_(client_fd_, total_transferred_);
                }
                return cleanupSpliceFileJob;
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
        PoolManager::deallocate(this);
        return;
    }

    // Start the first linked splice operation (file → pipe → socket).
    // If nothing was submitted, the error callback has fired — free the job.
    if (!startLinkedSplice(server)) {
        PoolManager::deallocate(this);
    }
}

void SpliceFileJob::createPipe() {
    if (pipe2(pipe_fds_, O_NONBLOCK | O_CLOEXEC) < 0) {
        Logger::getInstance().logError("SpliceFileJob: pipe2() failed: " +
                                       std::string(strerror(errno)));
        pipe_fds_[0] = pipe_fds_[1] = -1;
        return;
    }

    Logger::getInstance().logMessage("SpliceFileJob: Created pipe [" +
                                   std::to_string(pipe_fds_[0]) + ", " + std::to_string(pipe_fds_[1]) + "]");
}

bool SpliceFileJob::startLinkedSplice(Server& server) {
    // Determine chunk size for this splice operation.
    //
    // Clamp unconditionally: remaining_ is an exact byte count (the caller
    // resolves "to end of file" before constructing this job), so remaining_ == 0
    // means there is nothing left to send. Skipping the clamp in that case left
    // chunk_size at 64 KiB and spliced at EOF, which for an empty file produced a
    // zero-byte file→pipe leg followed by a pipe→socket leg that could never
    // complete — see the result == 0 handling in handleCompletion().
    size_t chunk_size = static_cast<size_t>(
        std::min<uint64_t>(SPLICE_CHUNK_SIZE, remaining_));

    if (chunk_size == 0) {
        // Nothing to transfer (empty file, or the whole range is already sent).
        // Complete immediately without submitting anything; for an empty file the
        // headers the caller already sent are the entire response.
        Logger::getInstance().logMessage("SpliceFileJob: Transfer complete fd=" +
                                       std::to_string(client_fd_) + ", total=" + std::to_string(total_transferred_));
        if (on_complete_) {
            on_complete_(client_fd_, total_transferred_);
        }
        return false;
    }

    Logger::getInstance().logMessage("SpliceFileJob: Starting linked splice - chunk_size=" +
                                   std::to_string(chunk_size) + ", offset=" + std::to_string(offset_));

    // Get SQE for file→pipe splice (stage 1)
    struct io_uring_sqe* sqe1 = server.registerJob(this);
    if (!sqe1) {
        if (on_error_) {
            on_error_(client_fd_, -ENOMEM);
        }
        return false;
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
        // sqe1 is already written into the SQE ring and cannot be removed.
        // Submit it now so we receive its completion, then handle the error
        // there once all in-flight operations have landed.
        error_pending_ = true;
        pending_operations_ = 1;
        server.submit();
        return true;
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
    return true;
}

bool SpliceFileJob::drainPipeToSocket(Server& server) {
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
        return false;
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
    return true;
}

bool SpliceFileJob::resubmit(Server& server) {
    // Resubmit the current operation
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (sqe) {
        prepareSqe(sqe);
        server.submit();
        return true;
    }
    if (on_error_) {
        on_error_(client_fd_, -ENOMEM);
    }
    return false;
}

void SpliceFileJob::closePipeWriteEnd() {
    if (pipe_fds_[1] >= 0) {
        close(pipe_fds_[1]);
        pipe_fds_[1] = -1;  // cleanup() skips it, so this is not a double close
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
