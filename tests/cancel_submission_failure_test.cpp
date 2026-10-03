#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>

#include "caduvelox/Server.hpp"
#include "caduvelox/http/HttpRouter.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/jobs/CancelJob.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include "caduvelox/util/PoolManager.hpp"

using namespace caduvelox;

/**
 * What happens when a cancel cannot be submitted (review items C5, L10).
 *
 * closeConnection() normally cancels an armed multishot recv and defers the
 * actual close until the cancel completes. Both legs of that -- allocating a
 * CancelJob and getting an SQE -- can fail, and submitRecvCancel() returns
 * false when either does. The fallback then frees the connection *immediately*
 * while the recv is still armed, because closing the fd does not terminate an
 * in-flight io_uring operation: it holds its own reference to the socket and
 * stays parked until data arrives, the peer closes, or the ring shuts down.
 *
 * That is the use-after-free C5 was filed for, and the generation counter in
 * HttpConnectionRecvHandler is the fix: the late completion validates
 * (connection, generation) through PoolManager before dereferencing anything.
 *
 * The comment in closeConnection() asserts all of this, and nothing tested it.
 * L10 proposed adding fault-injection seams to the production classes to get
 * here; none are needed. Both failure modes are reachable from the outside:
 * drain the CancelJob pool, or start a Server with a submission queue too
 * small to hold another SQE. Everything below drives the real connection
 * through its public entry points.
 *
 * These tests matter most under ASAN, where a missing validity gate is a
 * reported heap-use-after-free rather than a value that happens to survive.
 */
namespace {

class CancelSubmissionFailureTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(128));
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_), 0);

        router_.addRoute("GET", "^/hello$", [](const HttpRequest&, HttpResponse& res) {
            res.body = "hi";
        });
    }

    void TearDown() override {
        releaseCancelJobs();
        // sv_[0] belongs to the connection, which closes it on teardown.
        if (sv_[1] >= 0) ::close(sv_[1]);
    }

    // Hold every CancelJob the pool can produce, so the next allocation fails.
    // Draining rather than calling setCapacity(): a thread-local pool is sized
    // on first access, and another test in this binary may already have sized
    // this one, which would make a capacity override silently do nothing.
    size_t drainCancelJobs() {
        for (;;) {
            auto* job = PoolManager::allocate<CancelJob>(uint64_t{0});
            if (job == nullptr) break;
            held_cancel_jobs_.push_back(job);
        }
        return held_cancel_jobs_.size();
    }

    void releaseCancelJobs() {
        for (auto* job : held_cancel_jobs_) {
            PoolManager::deallocate(job);
        }
        held_cancel_jobs_.clear();
    }

    // Dispatch completions until the ring is quiet. Returns the number
    // dispatched. Jobs may free themselves, so nothing is touched afterwards.
    int pumpUntilQuiet(int timeout_ms = 300) {
        int dispatched = 0;
        for (;;) {
            struct __kernel_timespec ts{};
            ts.tv_nsec = static_cast<long long>(timeout_ms) * 1000 * 1000;
            struct io_uring_cqe* cqe = nullptr;
            if (io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts) < 0) break;

            auto* job = reinterpret_cast<IoJob*>(io_uring_cqe_get_data64(cqe));
            auto cleanup = job->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) (*cleanup)(job);
            ++dispatched;
            timeout_ms = 100;
        }
        return dispatched;
    }

    static std::string closingRequest() {
        return "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    }

    Server server_;
    HttpRouter router_;
    int sv_[2] = {-1, -1};
    std::vector<CancelJob*> held_cancel_jobs_;
};

// ---------------------------------------------------------------------------
// The CancelJob pool is empty
// ---------------------------------------------------------------------------

TEST_F(CancelSubmissionFailureTest, APoolExhaustedCancelFreesTheConnectionImmediately) {
    auto* conn = PoolManager::allocate<HttpConnectionJob>(
        sv_[0], server_, router_, 1024 * 1024, /*idle_timeout_ms=*/0);
    ASSERT_NE(conn, nullptr);
    const uint64_t gen = PoolManager::generation<HttpConnectionJob>(conn);

    conn->start();   // arms the multishot recv
    ASSERT_TRUE(PoolManager::isValid(conn, gen));

    ASSERT_GT(drainCancelJobs(), 0u) << "the CancelJob pool was already empty";

    // Answer one request that asks for the connection to be closed. The write
    // completion drives onResponseComplete() into closeConnection(), which is
    // where the cancel is attempted -- with the recv still armed.
    const std::string req = closingRequest();
    conn->handleDataReceived(req.data(), static_cast<ssize_t>(req.size()));
    pumpUntilQuiet();

    EXPECT_FALSE(PoolManager::isValid(conn, gen))
        << "with no CancelJob available the close cannot be deferred, so the "
           "connection has to be freed now -- deferring it would wait for a "
           "completion that was never submitted";
}

TEST_F(CancelSubmissionFailureTest, AParkedRecvDoesNotFeedTheConnectionThatReusedItsSlot) {
    // The dangerous shape of C5, and the only one the generation counter is
    // needed for. closeConnection() sets client_fd_ = -1 before deallocating,
    // and the pool does not scrub the slot, so a late completion against a
    // merely-freed connection hits handleDataReceived()'s own client_fd_ < 0
    // guard and does nothing. It is when the slot has been RECYCLED into a new
    // connection that the stale pointer becomes live again: client_fd_ is a
    // real socket belonging to somebody else, and bytes from the old peer are
    // parsed and answered as if that other client had sent them.
    auto* first = PoolManager::allocate<HttpConnectionJob>(
        sv_[0], server_, router_, 1024 * 1024, /*idle_timeout_ms=*/0);
    ASSERT_NE(first, nullptr);
    const uint64_t first_gen = PoolManager::generation<HttpConnectionJob>(first);

    first->start();
    ASSERT_GT(drainCancelJobs(), 0u);

    const std::string req = closingRequest();
    first->handleDataReceived(req.data(), static_cast<ssize_t>(req.size()));
    pumpUntilQuiet();
    ASSERT_FALSE(PoolManager::isValid(first, first_gen))
        << "precondition: the connection is freed with its recv still armed";

    // A second connection on its own socket pair. The pool hands back the slot
    // just released, which is what makes the stale pointer dangerous.
    int second_sv[2] = {-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, second_sv), 0);
    auto* second = PoolManager::allocate<HttpConnectionJob>(
        second_sv[0], server_, router_, 1024 * 1024, /*idle_timeout_ms=*/0);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(static_cast<void*>(second), static_cast<void*>(first))
        << "this test only means something if the slot was reused";
    second->start();

    // Wake the first connection's parked recv. Its completion carries the
    // recv job's handler, which still points at this slot -- now occupied by
    // `second`, which is reading a different socket.
    const std::string stale = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_GT(::send(sv_[1], stale.data(), stale.size(), MSG_NOSIGNAL), 0);
    pumpUntilQuiet();

    // Nothing may have been written to the second connection's client. A
    // response here means the stale completion was parsed and answered on
    // behalf of a client that sent nothing.
    struct timeval tv{ .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(second_sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[512];
    const ssize_t n = ::recv(second_sv[1], buf, sizeof(buf), 0);
    EXPECT_LE(n, 0) << "the recycled connection answered a request its own "
                       "client never sent: " << std::string(buf, n > 0 ? n : 0);

    // `second` owns second_sv[0]; close it through the connection's own path by
    // letting the fixture tear the server down, and release the peer here.
    ::close(second_sv[1]);
}

// ---------------------------------------------------------------------------
// The submission queue is full
// ---------------------------------------------------------------------------

// A user_data that is not a job pointer, for the filler SQEs below. The pump
// skips it instead of calling handleCompletion() on a bogus address.
constexpr uint64_t kFillerUserData = 0xF11153;

TEST(CancelSubmissionQueueFullTest, NoSqeForTheCancelAlsoFreesTheConnection) {
    static ConsoleLogger console_logger;
    Logger::setGlobalLogger(&console_logger);

    Server server;
    ASSERT_TRUE(server.init(8));

    int sv[2] = {-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    HttpRouter router;
    router.addRoute("GET", "^/hello$", [](const HttpRequest&, HttpResponse& res) {
        res.body = "hi";
    });

    auto* conn = PoolManager::allocate<HttpConnectionJob>(
        sv[0], server, router, 1024 * 1024, /*idle_timeout_ms=*/0);
    ASSERT_NE(conn, nullptr);
    const uint64_t gen = PoolManager::generation<HttpConnectionJob>(conn);

    conn->start();   // arms the multishot recv; its SQE is submitted

    // Answer a request that asks to close. Its WriteJob gets an SQE and is
    // submitted, so the queue is free again at this point.
    const std::string req =
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    conn->handleDataReceived(req.data(), static_cast<ssize_t>(req.size()));

    // Now fill the submission queue and leave it filled. Server::registerJob()
    // returns nullptr the moment io_uring_get_sqe() does -- it does not flush
    // and retry -- so this is the state in which submitRecvCancel() cannot get
    // an SQE however much pool capacity is available. The entries are prepped
    // as no-ops: an unsubmitted SQE holds whatever was in that ring slot
    // before, and something else submitting the queue later would hand the
    // kernel a garbage opcode.
    int filled = 0;
    for (;;) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(server.getRing());
        if (sqe == nullptr) break;
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, kFillerUserData);
        ++filled;
    }
    ASSERT_GT(filled, 0) << "could not fill the submission queue";

    // Already-submitted operations still complete, so the write completion
    // arrives and drives onResponseComplete() into closeConnection() -- which
    // now cannot get an SQE for the cancel.
    for (;;) {
        struct __kernel_timespec ts{};
        ts.tv_nsec = 200 * 1000 * 1000;
        struct io_uring_cqe* cqe = nullptr;
        if (io_uring_wait_cqe_timeout(server.getRing(), &cqe, &ts) < 0) break;
        const uint64_t data = io_uring_cqe_get_data64(cqe);
        if (data == kFillerUserData) {
            io_uring_cqe_seen(server.getRing(), cqe);
            continue;
        }
        auto* job = reinterpret_cast<IoJob*>(data);
        auto cleanup = job->handleCompletion(server, cqe);
        io_uring_cqe_seen(server.getRing(), cqe);
        if (cleanup) (*cleanup)(job);
    }

    EXPECT_FALSE(PoolManager::isValid(conn, gen))
        << "the connection must not be left waiting on a cancel completion "
           "that could not be submitted";

    if (sv[1] >= 0) ::close(sv[1]);
}

}  // namespace
