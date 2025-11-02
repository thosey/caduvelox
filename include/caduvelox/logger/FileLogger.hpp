#pragma once

#include "caduvelox/logger/Logger.hpp"
#include <fstream>
#include <string>

namespace caduvelox {

/**
 * File logger that writes logs to a file.
 * NOT thread-safe - should be used with AsyncLogger for concurrent access.
 * Automatically flushes after each write to ensure logs aren't lost on crash.
 */
class FileLogger : public Logger {
  public:
    /**
     * Create a file logger.
     * @param filepath Path to log file (will be created/appended to)
     * @param auto_flush If true, flush after each log message (safer but slower)
     */
    explicit FileLogger(const std::string& filepath, bool auto_flush = true);
    ~FileLogger();

    void logMessage(std::string_view msg) override;
    void logError(std::string_view msg) override;

    // Manually flush the log file
    void flush();
    
    // Reopen the log file (for log rotation via SIGHUP)
    void reopen();

  private:
    std::ofstream file_;
    bool auto_flush_;
    std::string filepath_;
};

} // namespace caduvelox
