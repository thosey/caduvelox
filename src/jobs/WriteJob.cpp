#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <liburing.h>
#include <cstring>
#include <cerrno>
#include <string>

// Default pool capacity for WriteJob (overridable at runtime via ServerConfig).
template<>
size_t caduvelox::PoolCapacityConfig<caduvelox::WriteJob>::capacity = 10000;

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
    if (job) {
        PoolManager::deallocate<WriteJob>(job);
    }
}

std::optional<IoJob::CleanupCallback> WriteJob::handleCompletion(Server& server, struct io_uring_cqe* cqe) {
    ssize_t result = cqe->res;
    
    if (result < 0) {
        if (on_error_) {
            on_error_(fd_, -result);
        }
        return cleanupWriteJob;
    }
    
    bytes_written_ += result;
    
    if (bytes_written_ >= total_length_) {
        // All data written successfully
        if (on_complete_) {
            on_complete_(fd_, bytes_written_);
        }
        return cleanupWriteJob;
    }
    
    // Partial write - continue with remaining data. submitWrite() owns the job
    // from here (it may free it), so nothing below may touch a member.
    submitWrite(server, "a resubmission after a partial write");
    return std::nullopt; // Continue with resubmitted job
}

void WriteJob::start(Server& server) {
    submitWrite(server, "the initial submission");
}

void WriteJob::prepareSqe(struct io_uring_sqe* sqe) {
    const char* remaining_data = data_ptr_ + bytes_written_;
    size_t remaining_length = total_length_ - bytes_written_;
    io_uring_prep_write(sqe, fd_, remaining_data, remaining_length, 0);
}

/**
 * Nothing was queued, so no completion will ever arrive and nobody downstream
 * will ever free this job. Free it here or the pool slot is gone for the life
 * of the process.
 *
 * The order matters: deallocate first, notify second. The error callback is
 * where a parent job tears itself down -- HTTPFileJob deallocates itself,
 * HttpConnectionJob closes the connection -- and running that while this job is
 * still alive means a callback can reach back into an object that is about to
 * disappear. Moving the callback out before the free keeps it valid across the
 * deallocation.
 */
void WriteJob::abandon(const std::string& message, int error) {
    Logger::getInstance().logError(message + ": error=" + std::to_string(error));

    ErrorCallback on_error = std::move(on_error_);
    const int fd = fd_;
    PoolManager::deallocate<WriteJob>(this);

    if (on_error) {
        on_error(fd, error);
    }
}

/**
 * Two different things can go wrong here, and they need opposite treatment --
 * which is why "free the job on every failure path" is not the fix it looks
 * like.
 *
 *   1. No SQE could be acquired. registerJob() returned nullptr, so nothing was
 *      queued and the server's in-flight count was never incremented. The job
 *      is dead: abandon() frees it. A full submission queue is transient, so
 *      this only happens after a flush has failed to free a slot.
 *
 *   2. io_uring_submit() failed after the SQE was filled in. This one must NOT
 *      free. liburing publishes queued entries to the shared ring -- it
 *      advances the tail -- *before* the io_uring_enter() that failed, so the
 *      entry is still there with this job's address in user_data, and the next
 *      submit() from anywhere in the process hands it to the kernel. Measured
 *      on 7.1, and pinned by WriteJobSubmitSemantics.FailedSubmitLeavesTheEntryQueued:
 *      a submit forced to fail left the tail advanced, and a later bare submit()
 *      -- preparing nothing new -- produced the completion carrying the original
 *      user_data. Freeing here would turn a leak into a use-after-free.
 *
 * Case 2 is not reported through on_error_ either. The operation is still live
 * and a completion is still coming; telling the caller it failed is precisely
 * what lets a parent tear itself down while its child is in flight.
 */
void WriteJob::submitWrite(Server& server, const char* what) {
    struct io_uring_sqe* sqe = server.registerJob(this);
    if (!sqe) {
        // The submission queue is full. Flushing it frees every slot at once,
        // so a momentary shortage should not fail the write.
        int flush_ret = server.submit();
        if (flush_ret < 0) {
            abandon(std::string("WriteJob: failed to flush the submission queue before ") + what,
                    -flush_ret);
            return;
        }

        sqe = server.registerJob(this);
        if (!sqe) {
            abandon(std::string("WriteJob: no SQE available for ") + what, EAGAIN);
            return;
        }
    }

    prepareSqe(sqe);
    int ret = server.submit();
    if (ret < 0) {
        // Case 2 above: queued, but not yet handed to the kernel. Not ours to free.
        Logger::getInstance().logError(
            std::string("WriteJob: io_uring_submit failed during ") + what +
            ": error=" + std::to_string(-ret) +
            "; the operation stays queued for the next submit");
    }
}

} // namespace caduvelox
