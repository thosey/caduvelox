#include <gtest/gtest.h>
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <chrono>

using namespace caduvelox;

/**
 * Test for Issue #4: AcceptJob Pool Leak on Errors
 * 
 * Verifies that AcceptJob objects are properly returned to the pool after
 * terminal errors (not just successful completions or multishot continuation).
 * 
 * Expected Result: This test should FAIL before the fix, showing that pool
 * entries are not returned after error conditions.
 */
class AcceptJobErrorCleanupTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        
        ASSERT_TRUE(job_server_.init(128));
        
        // Start event loop for processing completions
        event_loop_thread_ = std::thread([this]() {
            job_server_.run();
        });
        
        // Give event loop time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    void TearDown() override {
        stop_requested_ = true;
        job_server_.stop();
        
        if (event_loop_thread_.joinable()) {
            event_loop_thread_.join();
        }
    }
    
    Server job_server_;
    std::thread event_loop_thread_;
    std::atomic<bool> stop_requested_{false};
};

TEST_F(AcceptJobErrorCleanupTest, PoolEntriesReturnedOnInvalidFd) {
    // Get initial pool statistics
    size_t initial_available = PoolManager::available<AcceptJob>();
    size_t initial_allocated = PoolManager::allocated<AcceptJob>();
    
    std::cout << "Initial pool state - available: " << initial_available 
              << ", allocated: " << initial_allocated << std::endl;
    
    // Create and submit AcceptJobs with invalid file descriptor
    // This should cause immediate errors
    const int num_iterations = 50;
    int error_count = 0;
    
    for (int i = 0; i < num_iterations; i++) {
        int invalid_fd = -1;  // Invalid file descriptor
        
        auto* accept_job = PoolManager::allocate<AcceptJob>(
            invalid_fd
        );
        
        ASSERT_NE(accept_job, nullptr) << "Failed to allocate AcceptJob on iteration " << i;
        
        // Set error callback to track errors
        accept_job->start(job_server_);
        
        // Give time for the error to be processed
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    
    // Wait for all completions to be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Check final pool statistics
    size_t final_available = PoolManager::available<AcceptJob>();
    size_t final_allocated = PoolManager::allocated<AcceptJob>();
    
    std::cout << "Final pool state - available: " << final_available 
              << ", allocated: " << final_allocated << std::endl;
    std::cout << "Pool entries leaked: " << (initial_available - final_available) << std::endl;
    
    // The pool should have approximately the same number of available entries
    // (allowing some tolerance for timing)
    size_t leaked_entries = initial_available - final_available;
    
    // This assertion should FAIL before the fix is applied
    EXPECT_LE(leaked_entries, 5) 
        << "Pool entries were not returned after errors. "
        << "Leaked " << leaked_entries << " entries out of " << num_iterations << " iterations.";
    
    // Alternatively, check that allocated count returned to baseline
    EXPECT_LE(final_allocated, initial_allocated + 5)
        << "Too many AcceptJob entries still allocated after errors.";
}

TEST_F(AcceptJobErrorCleanupTest, PoolEntriesReturnedOnClosedSocket) {
    // Get initial pool statistics
    size_t initial_available = PoolManager::available<AcceptJob>();
    
    const int num_iterations = 30;
    
    for (int i = 0; i < num_iterations; i++) {
        // Create a socket, then immediately close it before accepting
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(server_fd, 0);
        
        // Immediately close the socket
        close(server_fd);
        
        // Try to use the closed socket for accept
        auto* accept_job = PoolManager::allocate<AcceptJob>(server_fd);
        ASSERT_NE(accept_job, nullptr);
        
        accept_job->start(job_server_);
        
        // Give time for error processing
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    
    // Wait for completions
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    size_t final_available = PoolManager::available<AcceptJob>();
    size_t leaked_entries = initial_available - final_available;
    
    std::cout << "Closed socket test - leaked entries: " << leaked_entries << std::endl;
    
    // This should FAIL before the fix
    EXPECT_LE(leaked_entries, 5)
        << "Pool entries leaked when accept fails on closed sockets.";
}

TEST_F(AcceptJobErrorCleanupTest, PoolRecoverAfterErrors) {
    // This test verifies the pool can be reused after errors
    size_t initial_available = PoolManager::available<AcceptJob>();
    
    // First round: cause errors
    for (int i = 0; i < 20; i++) {
        auto* job = PoolManager::allocate<AcceptJob>(-1);
        if (job) {
            job->start(job_server_);
        }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    size_t after_errors = PoolManager::available<AcceptJob>();
    
    // Second round: cause more errors (should reuse pool)
    for (int i = 0; i < 20; i++) {
        auto* job = PoolManager::allocate<AcceptJob>(-1);
        if (job) {
            job->start(job_server_);
        }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    size_t after_round2 = PoolManager::available<AcceptJob>();
    
    std::cout << "Pool recovery test - initial: " << initial_available 
              << ", after round 1: " << after_errors
              << ", after round 2: " << after_round2 << std::endl;
    
    // Pool should stabilize (not continue growing allocated count)
    // This should FAIL if entries are leaking
    EXPECT_LE(initial_available - after_round2, 10)
        << "Pool did not recover between error rounds, entries are leaking.";
}
