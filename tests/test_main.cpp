#include "caduvelox/logger/AsyncLogger.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <gtest/gtest.h>
#include <memory>

// Global async logger instance for tests
std::unique_ptr<caduvelox::AsyncLogger> global_async_logger;

int main(int argc, char **argv) {
    // Create async logger with console logger as delegate for tests
    auto consoleLogger = std::make_unique<caduvelox::ConsoleLogger>();
    global_async_logger = std::make_unique<caduvelox::AsyncLogger>(std::move(consoleLogger));
    
    caduvelox::Logger::setGlobalLogger(global_async_logger.get());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
