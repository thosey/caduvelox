#include <gtest/gtest.h>
#include "caduvelox/jobs/MultishotRecvJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace caduvelox;

namespace {

/**
 * Observable state shared with the test handler. The handler itself is copied
 * into the job, so all state lives here and the handler only holds a pointer.
 */
struct RecvHandlerState {
    int data_calls = 0;
    size_t data_bytes = 0;
    int error_calls = 0;
    int last_error = 0;
    int rearm_queries = 0;
    bool allow_rearm = true;
};

struct TestRecvHandler {
    RecvHandlerState* state;

    void onDataToken(ProvidedBufferToken& token) {
        ++state->data_calls;
        state->data_bytes += token.size();
    }

    void onError(int error) {
        ++state->error_calls;
        state->last_error = error;
    }

    bool shouldRearmRecv() {
        ++state->rearm_queries;
        return state->allow_rearm;
    }
};

using TestRecvJob = MultishotRecvJob<TestRecvHandler>;

} // namespace

// Give the test job its own pool so allocation counts are not shared with the
// production HttpConnectionRecvHandler instantiation.
template<>
size_t caduvelox::PoolCapacityConfig<TestRecvJob>::capacity = 16;

/**
 * Deterministic coverage for review item C1: the kernel can END a multishot recv
 * by clearing IORING_CQE_F_MORE, including on a completion that carried data.
 *
 * The end-to-end test in multishot_recv_rearm_test.cpp cannot reliably provoke
 * that condition (it depends on buffer-ring pressure and kernel internals), so
 * these tests synthesise the CQE and drive handleCompletion() directly — the
 * same technique shutdown_guard_test.cpp uses. The Server is initialised so
 * registerJob()/submit() work, but no event loop runs.
 */
class MultishotRecvRearmUnitTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(128));
    }

    void TearDown() override {
        if (sv_[0] >= 0) close(sv_[0]);
        if (sv_[1] >= 0) close(sv_[1]);
    }

    // Allocate a job from the pool reading from one end of a socketpair.
    TestRecvJob* makeJob() {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv_) != 0) {
            return nullptr;
        }
        return PoolManager::allocate<TestRecvJob>(
            sv_[0], TestRecvHandler{&state_}, *server_.getBufferRingCoordinator());
    }

    // A completion carrying `bytes` of data in buffer 0.
    // `more` controls IORING_CQE_F_MORE — false means the kernel ended the multishot.
    static struct io_uring_cqe dataCqe(int bytes, bool more) {
        struct io_uring_cqe cqe{};
        cqe.res = bytes;
        cqe.flags = IORING_CQE_F_BUFFER;  // buffer id 0
        if (more) {
            cqe.flags |= IORING_CQE_F_MORE;
        }
        return cqe;
    }

    Server server_;
    RecvHandlerState state_;
    int sv_[2] = {-1, -1};
};

// ---------------------------------------------------------------------------
// Normal continuation — the common case must be unaffected
// ---------------------------------------------------------------------------

TEST_F(MultishotRecvRearmUnitTest, StaysArmedWithoutRearmWhenMoreFlagSet) {
    TestRecvJob* job = makeJob();
    ASSERT_NE(job, nullptr);

    struct io_uring_cqe cqe = dataCqe(4, /*more=*/true);
    auto cleanup = job->handleCompletion(server_, &cqe);

    EXPECT_FALSE(cleanup.has_value()) << "job must not be freed while the kernel still owns it";
    EXPECT_EQ(state_.data_calls, 1);
    EXPECT_EQ(state_.rearm_queries, 0) << "no re-arm decision needed while F_MORE is set";
    EXPECT_EQ(state_.error_calls, 0);

    PoolManager::deallocate(job);
}

// ---------------------------------------------------------------------------
// C1: kernel ends the multishot on a data completion
// ---------------------------------------------------------------------------

TEST_F(MultishotRecvRearmUnitTest, RearmsWithWorkingRecvWhenKernelEndsMultishot) {
    TestRecvJob* job = makeJob();
    ASSERT_NE(job, nullptr);
    state_.allow_rearm = true;

    struct io_uring_cqe cqe = dataCqe(4, /*more=*/false);
    auto cleanup = job->handleCompletion(server_, &cqe);

    EXPECT_FALSE(cleanup.has_value()) << "a re-armed job must stay alive";
    EXPECT_EQ(state_.data_calls, 1) << "the data on the terminating CQE must still be delivered";
    EXPECT_EQ(state_.rearm_queries, 1);
    EXPECT_EQ(state_.error_calls, 0);

    // Prove the re-arm produced a real, working recv rather than just returning
    // nullopt: write to the peer and expect a completion addressed to this job.
    const char msg[] = "ping";
    ASSERT_EQ(write(sv_[1], msg, sizeof(msg) - 1), static_cast<ssize_t>(sizeof(msg) - 1));

    struct __kernel_timespec ts{};
    ts.tv_sec = 5;
    struct io_uring_cqe* real_cqe = nullptr;
    int ret = io_uring_wait_cqe_timeout(server_.getRing(), &real_cqe, &ts);

    ASSERT_EQ(ret, 0) << "no completion arrived — the recv was not actually re-armed";
    EXPECT_EQ(io_uring_cqe_get_data64(real_cqe), reinterpret_cast<uint64_t>(job))
        << "completion should be addressed to the same job object (owner's pointer stays valid)";
    EXPECT_EQ(real_cqe->res, 4) << "re-armed recv should deliver the bytes written by the peer";
    io_uring_cqe_seen(server_.getRing(), real_cqe);

    PoolManager::deallocate(job);
}

TEST_F(MultishotRecvRearmUnitTest, FreesJobWhenOwnerDeclinesRearm) {
    TestRecvJob* job = makeJob();
    ASSERT_NE(job, nullptr);
    state_.allow_rearm = false;  // e.g. connection is closing, or already freed

    const size_t allocated_before = PoolManager::allocated<TestRecvJob>();

    struct io_uring_cqe cqe = dataCqe(4, /*more=*/false);
    auto cleanup = job->handleCompletion(server_, &cqe);

    ASSERT_TRUE(cleanup.has_value()) << "an un-re-armed terminal completion must free the job";
    EXPECT_EQ(state_.data_calls, 1);
    EXPECT_EQ(state_.rearm_queries, 1);
    EXPECT_EQ(state_.error_calls, 1)
        << "the owner must be told the read stream ended so it drops its tracking pointer";
    EXPECT_EQ(state_.last_error, -ECANCELED);

    (*cleanup)(job);
    EXPECT_EQ(PoolManager::allocated<TestRecvJob>(), allocated_before - 1)
        << "pool slot must be reclaimed, not leaked";
}

// ---------------------------------------------------------------------------
// Terminal-CQE discipline on the error paths
// ---------------------------------------------------------------------------

TEST_F(MultishotRecvRearmUnitTest, FreesJobOnTerminalError) {
    TestRecvJob* job = makeJob();
    ASSERT_NE(job, nullptr);

    struct io_uring_cqe cqe{};
    cqe.res = -ECONNRESET;
    cqe.flags = 0;  // terminal

    auto cleanup = job->handleCompletion(server_, &cqe);

    ASSERT_TRUE(cleanup.has_value());
    EXPECT_EQ(state_.error_calls, 1);
    EXPECT_EQ(state_.last_error, -ECONNRESET);
    EXPECT_EQ(state_.rearm_queries, 0) << "a failed recv is reported, never re-armed";

    (*cleanup)(job);
}

TEST_F(MultishotRecvRearmUnitTest, KeepsJobAliveOnErrorThatDoesNotEndMultishot) {
    // An error CQE with F_MORE still set means the kernel retains this job as
    // SQE user_data — freeing it here would be a use-after-free on the next CQE.
    TestRecvJob* job = makeJob();
    ASSERT_NE(job, nullptr);

    struct io_uring_cqe cqe{};
    cqe.res = -ENOBUFS;
    cqe.flags = IORING_CQE_F_MORE;

    auto cleanup = job->handleCompletion(server_, &cqe);

    EXPECT_FALSE(cleanup.has_value())
        << "must not free a job the kernel still references";
    EXPECT_EQ(state_.error_calls, 1) << "the error should still be reported";
    EXPECT_EQ(state_.last_error, -ENOBUFS);

    PoolManager::deallocate(job);
}
