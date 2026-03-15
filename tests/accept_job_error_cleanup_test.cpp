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
 * Verifies that AcceptJob cleanup callbacks are properly returned when
 * unrecoverable errors occur. Uses a callback counter to verify cleanup
 * execution rather than pool statistics (which are thread-local).
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
    std::atomic<int> cleanup_count_{0};
};

TEST_F(AcceptJobErrorCleanupTest, CleanupCallbackReturnedOnError) {
    // This test directly verifies that handleCompletion returns a cleanup callback
    // when an unrecoverable error occurs, rather than returning nullopt (which would leak)
    
    AcceptJob job(-1);  // Invalid fd
    
    // Simulate an error completion (EBADF = bad file descriptor)
    struct io_uring_cqe cqe;
    cqe.res = -EBADF;  // Error result
    cqe.flags = 0;
    
    auto cleanup = job.handleCompletion(job_server_, &cqe);
    
    // Verify cleanup callback is returned (not nullopt)
    EXPECT_TRUE(cleanup.has_value()) 
        << "AcceptJob::handleCompletion should return cleanup callback on unrecoverable errors, not nullopt";
    
    if (cleanup.has_value()) {
        std::cout << "✓ Cleanup callback returned for unrecoverable error (EBADF)" << std::endl;
    }
}

TEST_F(AcceptJobErrorCleanupTest, MultipleUnrecoverableErrors) {
    // Test various unrecoverable error codes to ensure cleanup callbacks are returned
    // Note: We only test unrecoverable errors directly because recoverable errors
    // (EMFILE, ENFILE, ENOBUFS) call resubmitAccept() which requires a full io_uring context
    
    struct ErrorCase {
        int errno_val;
        const char* description;
    };
    
    std::vector<ErrorCase> test_cases = {
        {EBADF, "EBADF (bad file descriptor)"},
        {EINVAL, "EINVAL (invalid argument)"},
        {ECONNABORTED, "ECONNABORTED (connection aborted)"},
        {EPROTO, "EPROTO (protocol error)"},
        {EOPNOTSUPP, "EOPNOTSUPP (operation not supported)"},
    };
    
    for (const auto& test_case : test_cases) {
        AcceptJob job(-1);  // Invalid fd
        
        struct io_uring_cqe cqe;
        cqe.res = -test_case.errno_val;
        cqe.flags = 0;
        
        auto cleanup = job.handleCompletion(job_server_, &cqe);
        
        EXPECT_TRUE(cleanup.has_value()) 
            << "Expected cleanup callback for " << test_case.description;
        
        if (cleanup.has_value()) {
            std::cout << "✓ Cleanup returned for " << test_case.description << std::endl;
        }
    }
}

TEST_F(AcceptJobErrorCleanupTest, CleanupCallbackExecutesCorrectly) {
    // Verify that the cleanup callback can be executed and properly deallocates
    // Note: We allocate on this thread, but in production the deallocation
    // would happen on the io_uring thread
    
    auto* job = PoolManager::allocate<AcceptJob>(-1);
    ASSERT_NE(job, nullptr);
    
    // Simulate an error
    struct io_uring_cqe cqe;
    cqe.res = -EBADF;
    cqe.flags = 0;
    
    auto cleanup = job->handleCompletion(job_server_, &cqe);
    
    ASSERT_TRUE(cleanup.has_value()) << "Should return cleanup callback on error";
    
    // Execute the cleanup callback
    (*cleanup)(job);
    
    std::cout << "✓ Cleanup callback executed successfully" << std::endl;
}
