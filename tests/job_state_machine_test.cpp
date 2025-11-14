#include <gtest/gtest.h>
#include "caduvelox/util/JobStateMachine.hpp"
#include <thread>
#include <vector>
#include <atomic>

using namespace caduvelox;

class JobStateMachineTest : public ::testing::Test {
protected:
    JobStateMachine sm_;
};

// ============================================================================
// Basic State Transition Tests
// ============================================================================

TEST_F(JobStateMachineTest, InitialStateIsAvailable) {
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::AVAILABLE);
}

TEST_F(JobStateMachineTest, AcquireSucceedsFromAvailable) {
    EXPECT_TRUE(sm_.tryAcquire());
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::WORKING);
}

TEST_F(JobStateMachineTest, AcquireFailsFromWorking) {
    ASSERT_TRUE(sm_.tryAcquire());
    EXPECT_FALSE(sm_.tryAcquire());
}

TEST_F(JobStateMachineTest, ReleaseSucceedsFromWorking) {
    ASSERT_TRUE(sm_.tryAcquire());
    EXPECT_TRUE(sm_.tryRelease());
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::AVAILABLE);
}

TEST_F(JobStateMachineTest, DiscardSucceedsFromAvailable) {
    EXPECT_TRUE(sm_.tryDiscard());
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::DISCARDED);
}

TEST_F(JobStateMachineTest, DiscardWhileWorkingMarksForCleanup) {
    ASSERT_TRUE(sm_.tryAcquire());
    
    // Discard while working - should return false (worker will cleanup)
    EXPECT_FALSE(sm_.tryDiscard());
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::DISCARDED);
}

TEST_F(JobStateMachineTest, ReleaseAfterDiscardReturnsCleanupFlag) {
    ASSERT_TRUE(sm_.tryAcquire());
    ASSERT_FALSE(sm_.tryDiscard());  // Mark as discarded
    
    // Release should detect discard and return false (cleanup required)
    EXPECT_FALSE(sm_.tryRelease());
}

TEST_F(JobStateMachineTest, AcquireFailsAfterDiscard) {
    ASSERT_TRUE(sm_.tryDiscard());
    EXPECT_FALSE(sm_.tryAcquire());
}

// ============================================================================
// Complete Workflow Tests
// ============================================================================

TEST_F(JobStateMachineTest, NormalWorkflow) {
    // Acquire -> work -> release
    EXPECT_TRUE(sm_.tryAcquire());
    EXPECT_TRUE(sm_.tryRelease());
    
    // Can acquire again after release
    EXPECT_TRUE(sm_.tryAcquire());
}

TEST_F(JobStateMachineTest, DiscardIdleJob) {
    // Job never acquired - discard should cleanup immediately
    EXPECT_TRUE(sm_.tryDiscard());
}

TEST_F(JobStateMachineTest, DiscardWorkingJob) {
    ASSERT_TRUE(sm_.tryAcquire());
    
    // Discard while working
    EXPECT_FALSE(sm_.tryDiscard());
    
    // Worker detects discard on release
    EXPECT_FALSE(sm_.tryRelease());
}

// ============================================================================
// Concurrent Access Tests
// ============================================================================

TEST_F(JobStateMachineTest, ConcurrentAcquireOnlyOneSucceeds) {
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&]() {
            if (sm_.tryAcquire()) {
                success_count++;
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(success_count.load(), 1);
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::WORKING);
}

TEST_F(JobStateMachineTest, ConcurrentDiscardWhileWorking) {
    ASSERT_TRUE(sm_.tryAcquire());
    
    std::atomic<int> cleanup_by_discard{0};
    std::atomic<int> cleanup_by_worker{0};
    std::vector<std::thread> threads;
    
    // Multiple threads try to discard
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&]() {
            if (sm_.tryDiscard()) {
                cleanup_by_discard++;
            }
        });
    }
    
    // Worker releases
    threads.emplace_back([&]() {
        if (!sm_.tryRelease()) {
            cleanup_by_worker++;
        }
    });
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Exactly one thread should be responsible for cleanup
    EXPECT_EQ(cleanup_by_discard.load() + cleanup_by_worker.load(), 1);
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::DISCARDED);
}

TEST_F(JobStateMachineTest, RaceAcquireVsDiscard) {
    std::atomic<bool> acquired{false};
    std::atomic<bool> discard_cleanup{false};
    std::atomic<bool> worker_cleanup{false};
    
    // Thread 1: Try to acquire
    std::thread worker([&]() {
        if (sm_.tryAcquire()) {
            acquired = true;
            // Simulate some work
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (!sm_.tryRelease()) {
                worker_cleanup = true;
            }
        }
    });
    
    // Thread 2: Try to discard immediately
    std::thread closer([&]() {
        if (sm_.tryDiscard()) {
            discard_cleanup = true;
        }
    });
    
    worker.join();
    closer.join();
    
    // Verify cleanup was done exactly once
    int total_cleanup = (discard_cleanup ? 1 : 0) + (worker_cleanup ? 1 : 0);
    EXPECT_EQ(total_cleanup, 1);
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::DISCARDED);
}

TEST_F(JobStateMachineTest, MultipleAcquireReleaseDiscardCycles) {
    std::atomic<bool> should_stop{false};
    std::atomic<int> successful_cycles{0};
    std::atomic<int> cleanup_count{0};
    
    // Worker thread: repeatedly acquire and release
    std::thread worker([&]() {
        while (!should_stop.load()) {
            if (sm_.tryAcquire()) {
                // Simulate work
                std::this_thread::yield();
                if (sm_.tryRelease()) {
                    successful_cycles++;
                } else {
                    // Was discarded
                    cleanup_count++;
                    break;
                }
            }
        }
    });
    
    // Let worker run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Closer thread: discard
    std::thread closer([&]() {
        if (sm_.tryDiscard()) {
            cleanup_count++;
        }
        should_stop = true;
    });
    
    worker.join();
    closer.join();
    
    // Should have completed at least some cycles
    EXPECT_GT(successful_cycles.load(), 0);
    
    // Exactly one cleanup
    EXPECT_EQ(cleanup_count.load(), 1);
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::DISCARDED);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(JobStateMachineTest, DoubleDiscardIsIdempotent) {
    EXPECT_TRUE(sm_.tryDiscard());
    
    // Second discard should be safe and return false (already discarded)
    EXPECT_FALSE(sm_.tryDiscard());
}

TEST_F(JobStateMachineTest, ReleaseWithoutAcquireFails) {
    // Can't release if never acquired
    EXPECT_FALSE(sm_.tryRelease());
}

TEST_F(JobStateMachineTest, DiscardThenAcquireFails) {
    ASSERT_TRUE(sm_.tryDiscard());
    EXPECT_FALSE(sm_.tryAcquire());
    EXPECT_FALSE(sm_.tryRelease());
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_F(JobStateMachineTest, HighContentionAcquireRelease) {
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    constexpr int NUM_THREADS = 100;
    constexpr int ATTEMPTS_PER_THREAD = 100;
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < ATTEMPTS_PER_THREAD; ++j) {
                if (sm_.tryAcquire()) {
                    success_count++;
                    // Very brief "work"
                    std::this_thread::yield();
                    EXPECT_TRUE(sm_.tryRelease());
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Final state should be AVAILABLE since all releases succeeded
    EXPECT_EQ(sm_.getState(), JobStateMachine::State::AVAILABLE);
    EXPECT_GT(success_count.load(), 0);
}

TEST_F(JobStateMachineTest, StressTestWithDiscard) {
    std::atomic<bool> should_stop{false};
    std::atomic<int> successful_operations{0};
    std::atomic<int> cleanup_attempts{0};
    std::vector<std::thread> workers;
    
    // Spawn multiple workers
    for (int i = 0; i < 10; ++i) {
        workers.emplace_back([&]() {
            while (!should_stop.load()) {
                if (sm_.tryAcquire()) {
                    successful_operations++;
                    std::this_thread::yield();
                    if (!sm_.tryRelease()) {
                        cleanup_attempts++;
                        break;
                    }
                }
            }
        });
    }
    
    // Let workers run
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Discard
    if (sm_.tryDiscard()) {
        cleanup_attempts++;
    }
    should_stop = true;
    
    for (auto& t : workers) {
        t.join();
    }
    
    // Should have done some work
    EXPECT_GT(successful_operations.load(), 0);
    
    // Exactly one cleanup
    EXPECT_EQ(cleanup_attempts.load(), 1);
}
