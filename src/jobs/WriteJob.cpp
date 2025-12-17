#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <liburing.h>
#include <cstring>
#include <cerrno>
#include <string>

// Pool capacity specialization for WriteJob (extremely hot path)
template<>
constexpr size_t caduvelox::PoolManager::getPoolCapacity<caduvelox::WriteJob>() {
    return 10000; // Large pool - many concurrent write operations
}

namespace {
    void cleanupWriteJob(caduvelox::IoJob* job) {
        caduvelox::PoolManager::deallocate(static_cast<caduvelox::WriteJob*>(job));
    }
}

namespace caduvelox {

WriteJob::WriteJob(int fd, bool owns_data)
    : fd_(fd), owns_data_(owns_data), data_ptr_(nullptr),
      total_length_(0), bytes_written_(0) {
}

WriteJob* WriteJob::createFromPoolWithOwnedData(int fd, std::unique_ptr<char[]> data, size_t length,
                                               CompletionCallback on_complete,
                                               ErrorCallback on_error) {
    WriteJob* job = PoolManager::allocate<WriteJob>(fd, true);
    if (!job) {
        return nullptr; // Pool exhausted
    }
    
    job->is_pool_allocated_ = true;
    job->data_ptr_ = data.get();
    job->owned_data_ = std::move(data);
    job->total_length_ = length;
    job->bytes_written_ = 0;
    job->on_complete_ = std::move(on_complete);
    job->on_error_ = std::move(on_error);
    return job;
}

WriteJob* WriteJob::createFromPoolWithBorrowedData(int fd, const char* data, size_t length,
                                                  CompletionCallback on_complete,
                                                  ErrorCallback on_error) {
    WriteJob* job = PoolManager::allocate<WriteJob>(fd, false);
    if (!job) {
        return nullptr; // Pool exhausted
    }
    
    job->is_pool_allocated_ = true;
    job->data_ptr_ = data;
    job->total_length_ = length;
    job->bytes_written_ = 0;
    job->on_complete_ = std::move(on_complete);
    job->on_error_ = std::move(on_error);
    return job;
}

WriteJob* WriteJob::createFromPoolFromString(int fd, const std::string& data,
                                            CompletionCallback on_complete,
                                            ErrorCallback on_error) {
    auto buffer = std::make_unique<char[]>(data.size());
    std::memcpy(buffer.get(), data.data(), data.size());
    
    return createFromPoolWithOwnedData(fd, std::move(buffer), data.size(),
                                      std::move(on_complete), std::move(on_error));
}

void WriteJob::freePoolAllocated(WriteJob* job) {
    if (job && job->is_pool_allocated_) {
        PoolManager::deallocate<WriteJob>(job);
    }
}

std::optional<IoJob::CleanupCallback> WriteJob::handleCompletion(Server& server, struct io_uring_cqe* cqe) {
    ssize_t result = cqe->res;
    
    if (result < 0) {
        if (on_error_) {
            on_error_(fd_, -result);
        }
        // Return cleanup function if pool allocated
        if (is_pool_allocated_) {
            return cleanupWriteJob;
        }
        return std::nullopt; // Complete on error
    }
    
    bytes_written_ += result;
    
    if (bytes_written_ >= total_length_) {
        // All data written successfully
        if (on_complete_) {
            on_complete_(fd_, bytes_written_);
        }
        // Return cleanup function if pool allocated
        if (is_pool_allocated_) {
            return cleanupWriteJob;
        }
        return std::nullopt; // Complete on success
    }
    
    // Partial write - continue with remaining data
    resubmitWrite(server);
    return std::nullopt; // Continue with resubmitted job
}

void WriteJob::start(Server& server) {
    submitWrite(server);
}

void WriteJob::prepareSqe(struct io_uring_sqe* sqe) {
    const char* remaining_data = data_ptr_ + bytes_written_;
    size_t remaining_length = total_length_ - bytes_written_;
    io_uring_prep_write(sqe, fd_, remaining_data, remaining_length, 0);
}

void WriteJob::submitWrite(Server& server) {
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (!sqe) {
        int flush_ret = server.submit();
        if (flush_ret < 0) {
            Logger::getInstance().logError("WriteJob: Failed to flush pending submissions: error=" + std::to_string(-flush_ret));
            if (on_error_) {
                on_error_(fd_, -flush_ret);
            }
            return;
        }

        sqe = server.registerJob(this);
        if (!sqe) {
            Logger::getInstance().logError("WriteJob: Unable to acquire SQE for initial submission");
            if (on_error_) {
                on_error_(fd_, EAGAIN);
            }
            return;
        }
    }

    prepareSqe(sqe);
    int ret = server.submit();
    if (ret < 0) {
        Logger::getInstance().logError("WriteJob: io_uring_submit failed: error=" + std::to_string(-ret));
        if (on_error_) {
            on_error_(fd_, -ret);
        }
    }
}

void WriteJob::resubmitWrite(Server& server) {
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (!sqe) {
        int flush_ret = server.submit();
        if (flush_ret < 0) {
            Logger::getInstance().logError("WriteJob: Failed to flush pending submissions during resubmit: error=" + std::to_string(-flush_ret));
            if (on_error_) {
                on_error_(fd_, -flush_ret);
            }
            return;
        }

        sqe = server.registerJob(this);
        if (!sqe) {
            Logger::getInstance().logError("WriteJob: Unable to acquire SQE for resubmission");
            if (on_error_) {
                on_error_(fd_, EAGAIN);
            }
            return;
        }
    }

    prepareSqe(sqe);
    int ret = server.submit();
    if (ret < 0) {
        Logger::getInstance().logError("WriteJob: io_uring_submit failed on resubmit: error=" + std::to_string(-ret));
        if (on_error_) {
            on_error_(fd_, -ret);
        }
    }
}

} // namespace caduvelox
