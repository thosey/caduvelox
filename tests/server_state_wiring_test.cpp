#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/ServerState.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <atomic>

using namespace caduvelox;

/**
 * Unit tests for Server::bindToServerState / getServerState / isStopping / isAborting.
 *
 * No io_uring initialisation is required — the state pointer accessors are pure
 * in-memory operations that work on an uninitialised Server.
 */
class ServerStatePtrTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
    }

    // Declared before server_ so they are destroyed *after* it. ~Server() calls
    // stop(), which CASes through server_state_ — a TestBody-local atomic is
    // already dead by then, and ASAN reports the write as stack-use-after-return.
    std::atomic<ServerState> state_{ServerState::Running};
    std::atomic<ServerState> state2_{ServerState::Running};

    Server server_;
};

// --- Default behaviour (no external pointer installed) ---

TEST_F(ServerStatePtrTest, DefaultGetServerStateReturnsRunning) {
    EXPECT_EQ(server_.getServerState(), ServerState::Running);
}

TEST_F(ServerStatePtrTest, DefaultIsStoppingReturnsFalse) {
    EXPECT_FALSE(server_.isStopping());
}

TEST_F(ServerStatePtrTest, DefaultIsAbortingReturnsFalse) {
    EXPECT_FALSE(server_.isAborting());
}

// --- Redirect to an external atomic (multi-ring sharing) ---

TEST_F(ServerStatePtrTest, ReflectsRunningState) {
    state_.store(ServerState::Running);
    server_.bindToServerState(&state_);

    EXPECT_EQ(server_.getServerState(), ServerState::Running);
    EXPECT_FALSE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());
}

TEST_F(ServerStatePtrTest, ReflectsStoppingState) {
    state_.store(ServerState::Stopping);
    server_.bindToServerState(&state_);

    EXPECT_EQ(server_.getServerState(), ServerState::Stopping);
    EXPECT_TRUE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());
}

TEST_F(ServerStatePtrTest, ReflectsAbortingState) {
    state_.store(ServerState::Aborting);
    server_.bindToServerState(&state_);

    EXPECT_EQ(server_.getServerState(), ServerState::Aborting);
    EXPECT_FALSE(server_.isStopping());
    EXPECT_TRUE(server_.isAborting());
}

TEST_F(ServerStatePtrTest, ReflectsStoppedState) {
    state_.store(ServerState::Stopped);
    server_.bindToServerState(&state_);

    EXPECT_EQ(server_.getServerState(), ServerState::Stopped);
    EXPECT_FALSE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());
}

// --- Dynamic changes to the shared atomic are immediately visible ---

TEST_F(ServerStatePtrTest, DynamicTransitionRunningToStopping) {
    state_.store(ServerState::Running);
    server_.bindToServerState(&state_);

    EXPECT_FALSE(server_.isStopping());

    state_.store(ServerState::Stopping, std::memory_order_release);

    EXPECT_TRUE(server_.isStopping());
    EXPECT_EQ(server_.getServerState(), ServerState::Stopping);
}

TEST_F(ServerStatePtrTest, DynamicEscalationStoppingToAborting) {
    state_.store(ServerState::Stopping);
    server_.bindToServerState(&state_);

    EXPECT_TRUE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());

    state_.store(ServerState::Aborting, std::memory_order_release);

    EXPECT_FALSE(server_.isStopping());
    EXPECT_TRUE(server_.isAborting());
}

TEST_F(ServerStatePtrTest, DynamicTransitionToStopped) {
    state_.store(ServerState::Stopping);
    server_.bindToServerState(&state_);

    state_.store(ServerState::Stopped, std::memory_order_release);

    EXPECT_FALSE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());
    EXPECT_EQ(server_.getServerState(), ServerState::Stopped);
}

// --- Pointer can be swapped to a different atomic ---

TEST_F(ServerStatePtrTest, SwappingPointerReflectsNewState) {
    state_.store(ServerState::Running);
    state2_.store(ServerState::Stopping);

    server_.bindToServerState(&state_);
    EXPECT_FALSE(server_.isStopping());

    server_.bindToServerState(&state2_);
    EXPECT_TRUE(server_.isStopping());
}

// --- Multiple independent Server instances share one atomic without interfering ---

TEST_F(ServerStatePtrTest, TwoServersShareOneAtomic) {
    std::atomic<ServerState> shared{ServerState::Running};

    Server server_a;
    Server server_b;
    server_a.bindToServerState(&shared);
    server_b.bindToServerState(&shared);

    EXPECT_FALSE(server_a.isStopping());
    EXPECT_FALSE(server_b.isStopping());

    shared.store(ServerState::Stopping, std::memory_order_release);

    EXPECT_TRUE(server_a.isStopping());
    EXPECT_TRUE(server_b.isStopping());
}
