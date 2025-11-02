/**
 * Tests for logger functionality including file logging and SIGHUP rotation
 */

#include <gtest/gtest.h>
#include "caduvelox/logger/FileLogger.hpp"
#include "caduvelox/logger/AsyncLogger.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <signal.h>

using namespace caduvelox;
namespace fs = std::filesystem;

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temp directory for log files
        test_dir_ = fs::temp_directory_path() / "caduvelox_logger_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        // Clean up test directory
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    std::string readFile(const fs::path& path) {
        std::ifstream file(path);
        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        return content;
    }

    fs::path test_dir_;
};

TEST_F(LoggerTest, FileLoggerCreatesFile) {
    fs::path log_path = test_dir_ / "test.log";
    
    {
        FileLogger logger(log_path.string(), true);
        logger.logMessage("Test message");
    }
    
    ASSERT_TRUE(fs::exists(log_path));
    std::string content = readFile(log_path);
    EXPECT_TRUE(content.find("Test message") != std::string::npos);
    EXPECT_TRUE(content.find("[INFO]") != std::string::npos);
}

TEST_F(LoggerTest, FileLoggerAppendsToExistingFile) {
    fs::path log_path = test_dir_ / "append.log";
    
    {
        FileLogger logger(log_path.string(), true);
        logger.logMessage("First message");
    }
    
    {
        FileLogger logger(log_path.string(), true);
        logger.logMessage("Second message");
    }
    
    std::string content = readFile(log_path);
    EXPECT_TRUE(content.find("First message") != std::string::npos);
    EXPECT_TRUE(content.find("Second message") != std::string::npos);
}

TEST_F(LoggerTest, FileLoggerDistinguishesInfoAndError) {
    fs::path log_path = test_dir_ / "levels.log";
    
    {
        FileLogger logger(log_path.string(), true);
        logger.logMessage("Info message");
        logger.logError("Error message");
    }
    
    std::string content = readFile(log_path);
    EXPECT_TRUE(content.find("[INFO] Info message") != std::string::npos);
    EXPECT_TRUE(content.find("[ERROR] Error message") != std::string::npos);
}

TEST_F(LoggerTest, FileLoggerReopen) {
    fs::path log_path = test_dir_ / "reopen.log";
    fs::path rotated_path = test_dir_ / "reopen.log.1";
    
    FileLogger logger(log_path.string(), true);
    logger.logMessage("Before rotation");
    logger.flush();
    
    // Simulate logrotate: rename current log
    fs::rename(log_path, rotated_path);
    
    // Reopen should create new file
    logger.reopen();
    logger.logMessage("After rotation");
    logger.flush();
    
    // Old log should have first message
    ASSERT_TRUE(fs::exists(rotated_path));
    std::string old_content = readFile(rotated_path);
    EXPECT_TRUE(old_content.find("Before rotation") != std::string::npos);
    EXPECT_TRUE(old_content.find("After rotation") == std::string::npos);
    
    // New log should have second message and reopen notification
    ASSERT_TRUE(fs::exists(log_path));
    std::string new_content = readFile(log_path);
    EXPECT_TRUE(new_content.find("Log file reopened") != std::string::npos);
    EXPECT_TRUE(new_content.find("After rotation") != std::string::npos);
    EXPECT_TRUE(new_content.find("Before rotation") == std::string::npos);
}

TEST_F(LoggerTest, AsyncLoggerThreadSafety) {
    fs::path log_path = test_dir_ / "async.log";
    
    const int num_threads = 10;
    const int messages_per_thread = 100;
    
    {
        auto file_logger = std::make_unique<FileLogger>(log_path.string(), true);
        AsyncLogger async_logger(std::move(file_logger));
        
        // Spawn multiple threads writing concurrently
        std::vector<std::thread> threads;
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&async_logger, t, messages_per_thread]() {
                for (int i = 0; i < messages_per_thread; ++i) {
                    async_logger.logMessage("Thread " + std::to_string(t) + 
                                           " message " + std::to_string(i));
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        // AsyncLogger destructor will flush remaining messages
    }
    
    // All messages should be in the file
    std::string content = readFile(log_path);
    
    // Count how many messages were written
    size_t count = 0;
    size_t pos = 0;
    while ((pos = content.find("[INFO]", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    
    EXPECT_EQ(count, static_cast<size_t>(num_threads * messages_per_thread));
}

TEST_F(LoggerTest, ReopenWithAsyncLogger) {
    fs::path log_path = test_dir_ / "async_reopen.log";
    fs::path rotated_path = test_dir_ / "async_reopen.log.1";
    
    auto file_logger = std::make_unique<FileLogger>(log_path.string(), true);
    FileLogger* file_logger_ptr = file_logger.get();
    AsyncLogger async_logger(std::move(file_logger));
    
    async_logger.logMessage("Before rotation");
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let async write
    
    // Simulate logrotate
    fs::rename(log_path, rotated_path);
    
    // Reopen via saved pointer
    file_logger_ptr->reopen();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let reopen complete
    
    async_logger.logMessage("After rotation");
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let async write
    
    // Verify rotation worked
    ASSERT_TRUE(fs::exists(rotated_path));
    ASSERT_TRUE(fs::exists(log_path));
    
    std::string old_content = readFile(rotated_path);
    std::string new_content = readFile(log_path);
    
    EXPECT_TRUE(old_content.find("Before rotation") != std::string::npos);
    EXPECT_TRUE(new_content.find("After rotation") != std::string::npos);
    EXPECT_TRUE(new_content.find("Log file reopened") != std::string::npos);
}

TEST_F(LoggerTest, NoFlushMode) {
    fs::path log_path = test_dir_ / "noflush.log";
    
    {
        FileLogger logger(log_path.string(), false); // auto_flush = false
        logger.logMessage("Message 1");
        logger.logMessage("Message 2");
        // Without explicit flush, messages might not be written yet
        
        logger.flush(); // Explicit flush
    }
    
    std::string content = readFile(log_path);
    EXPECT_TRUE(content.find("Message 1") != std::string::npos);
    EXPECT_TRUE(content.find("Message 2") != std::string::npos);
}

// This test simulates the SIGHUP pattern used in production
TEST_F(LoggerTest, SIGHUPSimulation) {
    fs::path log_path = test_dir_ / "sighup.log";
    fs::path rotated_path = test_dir_ / "sighup.log.1";
    
    auto file_logger = std::make_unique<FileLogger>(log_path.string(), true);
    FileLogger* file_logger_ptr = file_logger.get();
    auto async_logger = std::make_unique<AsyncLogger>(std::move(file_logger));
    
    // Simulate application running and logging
    async_logger->logMessage("Log entry 1");
    async_logger->logMessage("Log entry 2");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Simulate logrotate moving the file (this would happen externally)
    fs::rename(log_path, rotated_path);
    
    // Simulate SIGHUP handler calling reopen()
    file_logger_ptr->reopen();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Continue logging to new file
    async_logger->logMessage("Log entry 3");
    async_logger->logMessage("Log entry 4");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Cleanup
    async_logger.reset();
    
    // Verify old log has old entries
    std::string old_content = readFile(rotated_path);
    EXPECT_TRUE(old_content.find("Log entry 1") != std::string::npos);
    EXPECT_TRUE(old_content.find("Log entry 2") != std::string::npos);
    EXPECT_TRUE(old_content.find("Log entry 3") == std::string::npos);
    
    // Verify new log has new entries and reopen message
    std::string new_content = readFile(log_path);
    EXPECT_TRUE(new_content.find("Log file reopened") != std::string::npos);
    EXPECT_TRUE(new_content.find("Log entry 3") != std::string::npos);
    EXPECT_TRUE(new_content.find("Log entry 4") != std::string::npos);
    EXPECT_TRUE(new_content.find("Log entry 1") == std::string::npos);
}
