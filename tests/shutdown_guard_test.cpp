#include <gtest/gtest.h>
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/KTLSJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/ServerState.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <atomic>

using namespace caduvelox;

/**
 * Verifies that AcceptJob and KTLSJob consult the shared ServerState at their
 * resubmit / continuation boundaries and stop extending themselves during shutdown.
 *
 * These tests call handleCompletion() directly so they work without a running
 * event loop. The Server is initialised so registerJob() and submit() are
 * available, but no ring thread is running — we only care about the return
 * value of handleCompletion() and the error callback.
 */
class ShutdownGuardTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(128));
    }

    void setServerState(ServerState state) {
        shared_state_.store(state, std::memory_order_release);
        server_.bindToServerState(&shared_state_);
    }

    Server server_;
    std::atomic<ServerState> shared_state_{ServerState::Running};
};

// ---------------------------------------------------------------------------
// AcceptJob — do not re-arm during shutdown
// ---------------------------------------------------------------------------

TEST_F(ShutdownGuardTest, AcceptDoesNotRearmOnMultishotTerminationDuringStopping) {
    setServerState(ServerState::Stopping);

    AcceptJob job(-1);
    struct io_uring_cqe cqe{};
    cqe.res = 0;
    cqe.flags = 0; // no IORING_CQE_F_MORE — multishot terminated

    auto cleanup = job.handleCompletion(server_, &cqe);
    EXPECT_TRUE(cleanup.has_value())
        << "AcceptJob should return cleanup (not resubmit) when multishot terminates during Stopping";
}

TEST_F(ShutdownGuardTest, AcceptDoesNotRearmOnMultishotTerminationDuringAborting) {
    setServerState(ServerState::Aborting);

    AcceptJob job(-1);
    struct io_uring_cqe cqe{};
    cqe.res = 0;
    cqe.flags = 0;

    auto cleanup = job.handleCompletion(server_, &cqe);
    EXPECT_TRUE(cleanup.has_value())
        << "AcceptJob should return cleanup (not resubmit) when multishot terminates during Aborting";
}

TEST_F(ShutdownGuardTest, AcceptSuppressesErrorCallbackDuringShutdown) {
    // Listener-close errors are expected during shutdown and should not propagate
    // to the error callback (which might log them as unexpected failures).
    setServerState(ServerState::Stopping);

    bool error_called = false;
    AcceptJob* job = AcceptJob::create(-1, nullptr, [&](int) { error_called = true; });
    ASSERT_NE(job, nullptr);

    struct io_uring_cqe cqe{};
    cqe.res = -EBADF; // socket closed — normal during shutdown
    cqe.flags = 0;

    auto cleanup = job->handleCompletion(server_, &cqe);

    EXPECT_FALSE(error_called)
        << "AcceptJob should suppress the error callback for expected shutdown errors";
    EXPECT_TRUE(cleanup.has_value())
        << "AcceptJob should still return a cleanup callback on error during Stopping";

    if (cleanup.has_value()) {
        (*cleanup)(job);
    }
}

TEST_F(ShutdownGuardTest, AcceptSuppressesErrorCallbackDuringAborting) {
    setServerState(ServerState::Aborting);

    bool error_called = false;
    AcceptJob* job = AcceptJob::create(-1, nullptr, [&](int) { error_called = true; });
    ASSERT_NE(job, nullptr);

    struct io_uring_cqe cqe{};
    cqe.res = -EBADF;
    cqe.flags = 0;

    auto cleanup = job->handleCompletion(server_, &cqe);

    EXPECT_FALSE(error_called)
        << "AcceptJob should suppress the error callback during Aborting";
    EXPECT_TRUE(cleanup.has_value());

    if (cleanup.has_value()) {
        (*cleanup)(job);
    }
}

// ---------------------------------------------------------------------------
// KTLSJob — fail fast during shutdown rather than extending poll chains
// ---------------------------------------------------------------------------

TEST_F(ShutdownGuardTest, KTLSJobFailsFastDuringStopping) {
    setServerState(ServerState::Stopping);

    bool error_called = false;
    int error_code = 0;
    // ssl_ctx=nullptr, timeout_ms=5000: shutdown check fires before SSL initialisation.
    auto* job = PoolManager::allocate<KTLSJob>(
        -1, nullptr, 5000,
        [](int, SSL*) {},
        [&](int, int code) { error_called = true; error_code = code; });
    ASSERT_NE(job, nullptr);

    // Simulate a poll completion that would normally advance the handshake.
    struct io_uring_cqe cqe{};
    cqe.res = 0;
    cqe.flags = 0;

    auto cleanup = job->handleCompletion(server_, &cqe);

    EXPECT_TRUE(error_called)
        << "KTLSJob should invoke the error callback when stopping during handshake";
    EXPECT_EQ(error_code, -ECANCELED)
        << "KTLSJob should report -ECANCELED when failing fast during shutdown";
    EXPECT_TRUE(cleanup.has_value())
        << "KTLSJob should return a cleanup callback after failing fast during Stopping";

    if (cleanup.has_value()) {
        (*cleanup)(job);
    }
}

TEST_F(ShutdownGuardTest, KTLSJobFailsFastDuringAborting) {
    setServerState(ServerState::Aborting);

    bool error_called = false;
    auto* job = PoolManager::allocate<KTLSJob>(
        -1, nullptr, 5000,
        [](int, SSL*) {},
        [&](int, int) { error_called = true; });
    ASSERT_NE(job, nullptr);

    struct io_uring_cqe cqe{};
    cqe.res = 0;
    cqe.flags = 0;

    auto cleanup = job->handleCompletion(server_, &cqe);

    EXPECT_TRUE(error_called)
        << "KTLSJob should invoke the error callback when aborting during handshake";
    EXPECT_TRUE(cleanup.has_value())
        << "KTLSJob should return a cleanup callback after failing fast during Aborting";

    if (cleanup.has_value()) {
        (*cleanup)(job);
    }
}
