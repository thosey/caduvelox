#include <gtest/gtest.h>
#include "caduvelox/jobs/SpliceFileJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>

using namespace caduvelox;

/**
 * Deterministic coverage for review item C4: splicing at end-of-file.
 *
 * Measured pre-fix behaviour on this kernel: an empty file still submitted a
 * 64 KiB file→pipe splice, which returned 0. A short result severs an
 * IOSQE_IO_LINK chain, so the kernel cancelled the pipe→socket partner and the
 * job reported -ECANCELED — every request for a zero-byte file failed and took
 * the connection down with it.
 *
 * The tests run against a real ring but without Server::run(): they pump
 * completions themselves with a bounded timeout, so if a splice ever does park
 * (the hang the review predicted, reachable if the partner is not cancelled)
 * the test fails with a diagnostic instead of hanging the whole suite — which
 * is precisely what such an operation does to Server::drainCompletions().
 */
class SpliceFileEofTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(128));
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_), 0);
    }

    void TearDown() override {
        if (file_fd_ >= 0) close(file_fd_);
        if (sv_[0] >= 0) close(sv_[0]);
        if (sv_[1] >= 0) close(sv_[1]);
        if (!file_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(file_path_, ec);
        }
    }

    // Create a file of `bytes` 'x' characters and open it for reading.
    void makeFile(const std::string& name, size_t bytes) {
        file_path_ = name;
        {
            std::ofstream ofs(file_path_, std::ios::binary | std::ios::trunc);
            ofs << std::string(bytes, 'x');
        }
        file_fd_ = open(file_path_.c_str(), O_RDONLY);
        ASSERT_GE(file_fd_, 0);
    }

    SpliceFileJob* makeJob(uint64_t length) {
        return SpliceFileJob::createFromPool(
            sv_[0], file_fd_, 0, length,
            [this](int, size_t bytes) {
                completed_ = true;
                bytes_transferred_ = bytes;
                done_ = true;
            },
            [this](int, int error) {
                errored_ = true;
                last_error_ = error;
                done_ = true;
            });
    }

    /**
     * Dispatch completions to `job` until it finishes. Every CQE on this ring
     * belongs to this job, so no general dispatch table is needed.
     * @return false if the wait timed out, i.e. an operation never completed.
     */
    bool pumpUntilDone(SpliceFileJob* job, int timeout_seconds = 5) {
        while (!done_) {
            struct __kernel_timespec ts{};
            ts.tv_sec = timeout_seconds;
            struct io_uring_cqe* cqe = nullptr;
            if (io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts) < 0) {
                return false;
            }
            auto cleanup = job->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) {
                (*cleanup)(job);
                break;  // job is freed — never touch it again
            }
        }
        return true;
    }

    // True if no completion is outstanding, i.e. the ring is fully drained and
    // Server::drainCompletions() would return rather than block.
    bool ringIsQuiet(int timeout_ms = 200) {
        struct __kernel_timespec ts{};
        ts.tv_nsec = static_cast<long long>(timeout_ms) * 1000 * 1000;
        struct io_uring_cqe* cqe = nullptr;
        return io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts) < 0;
    }

    Server server_;
    int sv_[2] = {-1, -1};
    int file_fd_ = -1;
    std::string file_path_;

    bool done_ = false;
    bool completed_ = false;
    bool errored_ = false;
    size_t bytes_transferred_ = 0;
    int last_error_ = 0;
};

// ---------------------------------------------------------------------------
// Empty file: nothing to splice at all
// ---------------------------------------------------------------------------

TEST_F(SpliceFileEofTest, EmptyFileCompletesWithoutSubmittingAnySplice) {
    makeFile("./splice_eof_empty.bin", 0);

    const size_t allocated_before = PoolManager::allocated<SpliceFileJob>();

    SpliceFileJob* job = makeJob(/*length=*/0);
    ASSERT_NE(job, nullptr);
    job->start(server_);

    EXPECT_TRUE(completed_) << "an empty body is a successful transfer, not an error";
    EXPECT_FALSE(errored_);
    EXPECT_EQ(bytes_transferred_, 0u);

    // The whole point of the fix: nothing is submitted for a zero-byte range.
    // Before it, a 64 KiB file->pipe splice went out at EOF and the resulting
    // short completion tore down the linked pair.
    EXPECT_TRUE(ringIsQuiet()) << "no operation should have been submitted";

    EXPECT_EQ(PoolManager::allocated<SpliceFileJob>(), allocated_before)
        << "the job must be back in the pool, not stranded";
}

// ---------------------------------------------------------------------------
// Truncated file: EOF arrives with bytes still owed
// ---------------------------------------------------------------------------

TEST_F(SpliceFileEofTest, TruncatedFileEndsTransferInsteadOfHanging) {
    // The file holds exactly one full chunk but the job was told to send more —
    // the state the server reaches when a file shrinks after fstat() fixed
    // Content-Length.
    //
    // The sizes matter. The first pair must transfer a FULL chunk, otherwise its
    // short result severs the link and the second pair is never reached. With
    // exactly SPLICE_CHUNK_SIZE on disk, pair 1 succeeds completely and pair 2
    // starts at EOF — which is the res == 0 case under test.
    makeFile("./splice_eof_truncated.bin", 64 * 1024);

    const size_t allocated_before = PoolManager::allocated<SpliceFileJob>();

    SpliceFileJob* job = makeJob(/*length=*/100000);
    ASSERT_NE(job, nullptr);
    job->start(server_);

    ASSERT_TRUE(pumpUntilDone(job))
        << "an operation never completed — the pipe->socket leg is parked on an "
           "empty pipe, which is what hangs the connection and the shutdown drain";

    EXPECT_TRUE(errored_)
        << "a short body must be reported as an error so the connection is closed, "
           "not reused with a desynced response stream";
    EXPECT_FALSE(completed_);
    EXPECT_EQ(last_error_, EIO);

    EXPECT_TRUE(ringIsQuiet()) << "nothing may still be in flight after teardown";
    EXPECT_EQ(PoolManager::allocated<SpliceFileJob>(), allocated_before)
        << "the job must be back in the pool, not stranded";

    // No assertion on what reached the socket: a short file->pipe result severs
    // the IOSQE_IO_LINK, so the kernel cancels the pipe->socket leg and the
    // prefix that was read stays in the pipe. The client sees a truncated body
    // either way, which is why this has to be reported as an error.
}

// ---------------------------------------------------------------------------
// Regression guard: the ordinary path must be unaffected
// ---------------------------------------------------------------------------

TEST_F(SpliceFileEofTest, WholeFileStillTransfersNormally) {
    makeFile("./splice_eof_whole.bin", 1000);

    SpliceFileJob* job = makeJob(/*length=*/1000);
    ASSERT_NE(job, nullptr);
    job->start(server_);

    ASSERT_TRUE(pumpUntilDone(job));

    EXPECT_TRUE(completed_);
    EXPECT_FALSE(errored_);
    EXPECT_EQ(bytes_transferred_, 1000u);

    char buf[2048];
    ssize_t n = recv(sv_[1], buf, sizeof(buf), MSG_DONTWAIT);
    EXPECT_EQ(n, 1000);
}
