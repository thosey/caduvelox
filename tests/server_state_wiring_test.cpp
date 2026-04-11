#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/ServerState.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <atomic>

using namespace caduvelox;

/**
 * Unit tests for Server::bindToServerState / getServerState / isStopping / isAborting.
 *
 * These tests exercise the state-pointer API added in Step 2 of SERVER_STATE_PLAN.md.
 * No io_uring initialisation is required — the state pointer accessors are pure
 * in-memory operations that work on an uninitialised Server.
 */
class ServerStatePtrTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
    }

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
    std::atomic<ServerState> state{ServerState::Running};
    server_.bindToServerState(&state);

    EXPECT_EQ(server_.getServerState(), ServerState::Running);
    EXPECT_FALSE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());
}

TEST_F(ServerStatePtrTest, ReflectsStoppingState) {
    std::atomic<ServerState> state{ServerState::Stopping};
    server_.bindToServerState(&state);

    EXPECT_EQ(server_.getServerState(), ServerState::Stopping);
    EXPECT_TRUE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());
}

TEST_F(ServerStatePtrTest, ReflectsAbortingState) {
    std::atomic<ServerState> state{ServerState::Aborting};
    server_.bindToServerState(&state);

    EXPECT_EQ(server_.getServerState(), ServerState::Aborting);
    EXPECT_FALSE(server_.isStopping());
    EXPECT_TRUE(server_.isAborting());
}

TEST_F(ServerStatePtrTest, ReflectsStoppedState) {
    std::atomic<ServerState> state{ServerState::Stopped};
    server_.bindToServerState(&state);

    EXPECT_EQ(server_.getServerState(), ServerState::Stopped);
    EXPECT_FALSE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());
}

// --- Dynamic changes to the shared atomic are immediately visible ---

TEST_F(ServerStatePtrTest, DynamicTransitionRunningToStopping) {
    std::atomic<ServerState> state{ServerState::Running};
    server_.bindToServerState(&state);

    EXPECT_FALSE(server_.isStopping());

    state.store(ServerState::Stopping, std::memory_order_release);

    EXPECT_TRUE(server_.isStopping());
    EXPECT_EQ(server_.getServerState(), ServerState::Stopping);
}

TEST_F(ServerStatePtrTest, DynamicEscalationStoppingToAborting) {
    std::atomic<ServerState> state{ServerState::Stopping};
    server_.bindToServerState(&state);

    EXPECT_TRUE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());

    state.store(ServerState::Aborting, std::memory_order_release);

    EXPECT_FALSE(server_.isStopping());
    EXPECT_TRUE(server_.isAborting());
}

TEST_F(ServerStatePtrTest, DynamicTransitionToStopped) {
    std::atomic<ServerState> state{ServerState::Stopping};
    server_.bindToServerState(&state);

    state.store(ServerState::Stopped, std::memory_order_release);

    EXPECT_FALSE(server_.isStopping());
    EXPECT_FALSE(server_.isAborting());
    EXPECT_EQ(server_.getServerState(), ServerState::Stopped);
}

// --- Pointer can be swapped to a different atomic ---

TEST_F(ServerStatePtrTest, SwappingPointerReflectsNewState) {
    std::atomic<ServerState> state1{ServerState::Running};
    std::atomic<ServerState> state2{ServerState::Stopping};

    server_.bindToServerState(&state1);
    EXPECT_FALSE(server_.isStopping());

    server_.bindToServerState(&state2);
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
