#include <gtest/gtest.h>
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace caduvelox;

/**
 * Who frees a job when submission fails (review items H2 and H3).
 *
 * There are two different failures hiding under the one phrase "submission
 * failed", and they need opposite treatment:
 *
 *   1. No SQE could be acquired. Nothing was queued, no completion will ever
 *      arrive, and so nobody downstream will ever free the job. Whoever
 *      created it has to, or the pool slot is gone for the life of the
 *      process.
 *
 *   2. io_uring_submit() failed after the SQE was filled in. Here the entry is
 *      already published to the shared submission ring with the job's address
 *      in user_data, and the next submit() from anywhere in the process hands
 *      it to the kernel. Freeing is a use-after-free, not a fix.
 *
 * FailedSubmitLeavesTheEntryQueued pins the kernel/liburing behaviour that
 * case 2 rests on. SqeExhaustionDoesNotStrandTheHeaderWriteJob measures case 1
 * where it is actually reachable: the header write in HTTPFileJob asks for one
 * SQE and gives up if it does not get one.
 *
 * These run against a real ring but without Server::run(), pumping completions
 * by hand, and every assertion is a pool-occupancy count rather than a log
 * message -- a leak is a slot that never comes back.
 */
namespace {

// Fills submission-queue slots without submitting them. prepareSqe() writes a
// nop so the entries are harmless when something else eventually flushes them.
class NopJob : public IoJob {
public:
    void prepareSqe(struct io_uring_sqe* sqe) override { io_uring_prep_nop(sqe); }
    std::optional<CleanupCallback> handleCompletion(Server&, struct io_uring_cqe*) override {
        ++completions;
        return std::nullopt;  // stack-allocated; nothing to free
    }
    int completions = 0;
};

class WriteJobOwnershipTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        // Deliberately shallow: the point of these tests is to run the ring out
        // of submission slots, and a 256-deep ring makes that tedious.
        ASSERT_TRUE(server_.init(8));
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_), 0);
        dir_ = std::filesystem::temp_directory_path() /
               ("cadu_h23_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        if (sv_[0] >= 0) close(sv_[0]);
        if (sv_[1] >= 0) close(sv_[1]);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::string makeRegularFile(const std::string& name, const std::string& contents) {
        const auto p = dir_ / name;
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs << contents;
        return p.string();
    }

    // Take every remaining submission slot, leaving the queue full but
    // unsubmitted -- the state a busy ring is in between completions.
    size_t fillSubmissionQueue(NopJob& filler) {
        size_t taken = 0;
        while (struct io_uring_sqe* sqe = server_.registerJob(&filler)) {
            filler.prepareSqe(sqe);
            ++taken;
        }
        return taken;
    }

    HTTPFileJob* makeFileJob(const std::string& path) {
        return HTTPFileJob::createFromPool(
            sv_[0], path, HttpResponse{},
            [this](int, size_t bytes) { completed_ = true; bytes_sent_ = bytes; done_ = true; },
            [this](int, int error)    { errored_ = true; last_error_ = error;  done_ = true; });
    }

    // Dispatch completions until the job reports done, or until the ring goes
    // quiet. Mirrors Server::handleCompletion().
    bool pumpUntilDone(int timeout_seconds = 3) {
        while (!done_) {
            struct __kernel_timespec ts{};
            ts.tv_sec = timeout_seconds;
            struct io_uring_cqe* cqe = nullptr;
            if (io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts) < 0) {
                return false;
            }
            auto* child = reinterpret_cast<IoJob*>(io_uring_cqe_get_data64(cqe));
            auto cleanup = child->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) {
                (*cleanup)(child);
            }
        }
        return true;
    }

    // Reap whatever else is sitting in the ring, so that leftover filler nops
    // cannot be mistaken for the job under test.
    void drainRemaining() {
        for (;;) {
            struct io_uring_cqe* cqe = nullptr;
            if (io_uring_peek_cqe(server_.getRing(), &cqe) != 0) {
                if (io_uring_submit(server_.getRing()) <= 0) break;
                struct __kernel_timespec ts{};
                ts.tv_nsec = 50 * 1000 * 1000;
                if (io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts) < 0) break;
            }
            auto* child = reinterpret_cast<IoJob*>(io_uring_cqe_get_data64(cqe));
            auto cleanup = child->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) {
                (*cleanup)(child);
            }
        }
    }

    std::string clientBytes(int timeout_ms = 300) {
        std::string out;
        for (;;) {
            struct pollfd pfd{sv_[1], POLLIN, 0};
            if (poll(&pfd, 1, timeout_ms) <= 0) break;
            char buf[4096];
            ssize_t n = recv(sv_[1], buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
            timeout_ms = 50;
        }
        return out;
    }

    Server server_;
    int sv_[2] = {-1, -1};
    std::filesystem::path dir_;

    bool done_ = false;
    bool completed_ = false;
    bool errored_ = false;
    size_t bytes_sent_ = 0;
    int last_error_ = 0;
};

// ---------------------------------------------------------------------------
// The kernel behaviour that decides which failure may free a job
// ---------------------------------------------------------------------------

/**
 * liburing publishes queued entries to the shared ring -- it advances the tail
 * -- *before* the io_uring_enter() that may fail. So a failed submit does not
 * unqueue anything: the entry is still there, still carrying the job's address,
 * and the next submit() from anywhere hands it to the kernel.
 *
 * That is why WriteJob does not free itself when submit() fails, and this test
 * exists so that the assumption is checked rather than remembered. It uses a
 * raw ring: the failure is forced by pointing enter(2) at a file descriptor
 * that is not an io_uring, which fails the syscall without disturbing the
 * queue.
 */
TEST(WriteJobSubmitSemantics, FailedSubmitLeavesTheEntryQueued) {
    struct io_uring ring{};
    ASSERT_EQ(io_uring_queue_init(8, &ring, 0), 0);

    const int real_fd = ring.ring_fd;
    const int dud = open("/dev/null", O_RDONLY);
    ASSERT_GE(dud, 0);

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(sqe, nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 0xD00Dul);

    ring.ring_fd = dud;
    ring.enter_ring_fd = dud;
    const int failed = io_uring_submit(&ring);
    ring.ring_fd = real_fd;
    ring.enter_ring_fd = real_fd;

    ASSERT_LT(failed, 0) << "expected the forced submit to fail";

    // Nothing new is prepared here. If the failed submit had discarded the
    // entry there would be nothing left to send.
    const int resubmitted = io_uring_submit(&ring);
    EXPECT_EQ(resubmitted, 1)
        << "a failed io_uring_submit() left nothing queued. If that is really "
           "true on this kernel, WriteJob may free itself on the submit-failure "
           "path -- and today it deliberately does not.";

    struct io_uring_cqe* cqe = nullptr;
    struct __kernel_timespec ts{};
    ts.tv_sec = 3;
    ASSERT_EQ(io_uring_wait_cqe_timeout(&ring, &cqe, &ts), 0);
    EXPECT_EQ(io_uring_cqe_get_data64(cqe), 0xD00Dul)
        << "the completion carries the user_data of the entry that 'failed' to "
           "submit, so freeing the job at that point would leave a dangling "
           "pointer in the ring";
    io_uring_cqe_seen(&ring, cqe);

    close(dud);
    io_uring_queue_exit(&ring);
}

// ---------------------------------------------------------------------------
// The reachable leak: one SQE attempt, no retry, no free
// ---------------------------------------------------------------------------

/**
 * startSendingHeaders() asked for a single SQE and, if the ring had none to
 * give, abandoned the WriteJob it had just taken from the pool. Nothing was
 * queued, so no completion ever came to free it: the slot was gone for the life
 * of the process. Under sustained submission pressure -- which is the only
 * condition that produces this branch -- the write pool drains permanently.
 */
TEST_F(WriteJobOwnershipTest, SqeExhaustionDoesNotStrandTheHeaderWriteJob) {
    const std::string path = makeRegularFile("payload.txt", "the quick brown fox");

    const size_t writes_before = PoolManager::allocated<WriteJob>();
    const size_t files_before  = PoolManager::allocated<HTTPFileJob>();

    HTTPFileJob* job = makeFileJob(path);
    ASSERT_NE(job, nullptr);

    NopJob filler;
    ASSERT_GT(fillSubmissionQueue(filler), 0u) << "could not fill the submission queue";

    job->start(server_);
    pumpUntilDone();
    drainRemaining();

    EXPECT_EQ(PoolManager::allocated<WriteJob>(), writes_before)
        << "a WriteJob was taken from the pool and never returned. The header "
           "write could not get a submission slot, so nothing was queued and no "
           "completion will ever arrive to free it -- the slot is lost until the "
           "process exits.";
    EXPECT_EQ(PoolManager::allocated<HTTPFileJob>(), files_before)
        << "the file job leaked alongside it";
}

/**
 * The same pressure, but checking the outcome rather than the accounting: a
 * full submission queue is a transient condition, and flushing it frees every
 * slot at once. Giving up on the first refusal turns a momentary shortage into
 * a failed request.
 */
TEST_F(WriteJobOwnershipTest, AFullSubmissionQueueDoesNotFailTheRequest) {
    const std::string body = "the quick brown fox";
    const std::string path = makeRegularFile("payload.txt", body);

    HTTPFileJob* job = makeFileJob(path);
    ASSERT_NE(job, nullptr);

    NopJob filler;
    ASSERT_GT(fillSubmissionQueue(filler), 0u);

    job->start(server_);
    ASSERT_TRUE(pumpUntilDone()) << "the transfer never finished";

    EXPECT_TRUE(completed_) << "the request failed with error " << last_error_
                            << " because the submission queue happened to be full "
                               "at the moment the headers were written";
    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 200"), std::string::npos) << "got:\n" << sent;
    EXPECT_NE(sent.find(body), std::string::npos) << "body missing from:\n" << sent;
}

// ---------------------------------------------------------------------------
// Guards: the ordinary paths must still balance
// ---------------------------------------------------------------------------

TEST_F(WriteJobOwnershipTest, AnOrdinaryTransferReturnsEveryPooledJob) {
    const std::string body = "the quick brown fox";
    const std::string path = makeRegularFile("payload.txt", body);

    const size_t writes_before = PoolManager::allocated<WriteJob>();
    const size_t files_before  = PoolManager::allocated<HTTPFileJob>();

    HTTPFileJob* job = makeFileJob(path);
    ASSERT_NE(job, nullptr);
    job->start(server_);
    ASSERT_TRUE(pumpUntilDone());
    drainRemaining();

    EXPECT_TRUE(completed_);
    EXPECT_EQ(PoolManager::allocated<WriteJob>(), writes_before);
    EXPECT_EQ(PoolManager::allocated<HTTPFileJob>(), files_before);
}

/**
 * The error path allocates a WriteJob of its own for the response body, and it
 * has to come back too.
 */
TEST_F(WriteJobOwnershipTest, AFailedOpenReturnsEveryPooledJob) {
    const size_t writes_before = PoolManager::allocated<WriteJob>();
    const size_t files_before  = PoolManager::allocated<HTTPFileJob>();

    HTTPFileJob* job = makeFileJob((dir_ / "absent.txt").string());
    ASSERT_NE(job, nullptr);
    job->start(server_);
    ASSERT_TRUE(pumpUntilDone());
    drainRemaining();

    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 404"), std::string::npos) << "got:\n" << sent;
    EXPECT_EQ(PoolManager::allocated<WriteJob>(), writes_before);
    EXPECT_EQ(PoolManager::allocated<HTTPFileJob>(), files_before);
}

/**
 * A write larger than the socket buffer comes back short, and WriteJob
 * resubmits the remainder from inside its own completion handler -- the one
 * submission path that is exercised in ordinary traffic. It must deliver every
 * byte and still balance the pool.
 */
TEST_F(WriteJobOwnershipTest, APartialWriteResubmitsAndDeliversEverything) {
    const size_t writes_before = PoolManager::allocated<WriteJob>();

    // A small send buffer plus a payload far larger than it guarantees the first
    // write comes back short; without that the kernel may take the whole thing
    // in one go and the resubmission path would never run.
    const int sndbuf = 8 * 1024;
    ASSERT_EQ(setsockopt(sv_[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)), 0);
    const std::string payload(512 * 1024, 'x');

    bool write_done = false;
    size_t written = 0;
    int write_error = 0;
    auto* job = WriteJob::createFromPoolFromString(
        sv_[0], payload,
        [&](int, size_t n) { written = n; write_done = true; },
        [&](int, int err)  { write_error = err; write_done = true; });
    ASSERT_NE(job, nullptr);

    const IoJob* watched = job;
    int completions_for_job = 0;

    job->start(server_);

    // Read as the server writes; otherwise the socket fills and neither side
    // makes progress.
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!write_done && std::chrono::steady_clock::now() < deadline) {
        struct io_uring_cqe* cqe = nullptr;
        if (io_uring_peek_cqe(server_.getRing(), &cqe) == 0) {
            auto* child = reinterpret_cast<IoJob*>(io_uring_cqe_get_data64(cqe));
            if (child == watched) {
                ++completions_for_job;
            }
            auto cleanup = child->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) {
                (*cleanup)(child);
            }
        }
        char buf[64 * 1024];
        ssize_t n = recv(sv_[1], buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            received.append(buf, static_cast<size_t>(n));
        }
    }

    ASSERT_TRUE(write_done) << "the write never finished; error=" << write_error;
    ASSERT_GT(completions_for_job, 1)
        << "the whole payload went out in a single write, so this test never "
           "exercised the resubmission path it exists to cover";
    EXPECT_EQ(written, payload.size()) << "resubmission dropped part of the payload";

    while (received.size() < payload.size()) {
        struct pollfd pfd{sv_[1], POLLIN, 0};
        if (poll(&pfd, 1, 300) <= 0) break;
        char buf[64 * 1024];
        ssize_t n = recv(sv_[1], buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) break;
        received.append(buf, static_cast<size_t>(n));
    }
    EXPECT_EQ(received.size(), payload.size());
    EXPECT_EQ(PoolManager::allocated<WriteJob>(), writes_before)
        << "the resubmitted job was not returned to the pool";
}

}  // namespace
