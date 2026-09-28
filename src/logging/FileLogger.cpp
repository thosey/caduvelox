#include "caduvelox/logger/FileLogger.hpp"
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>

namespace caduvelox {

FileLogger::FileLogger(const std::string& filepath, bool auto_flush)
    : auto_flush_(auto_flush), filepath_(filepath) {
    file_.open(filepath, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "FileLogger: Failed to open log file: " << filepath << std::endl;
    }
}

FileLogger::~FileLogger() {
    if (file_.is_open()) {
        file_.close();
    }
}

void FileLogger::logMessage(std::string_view msg) {
    if (file_.is_open()) {
        // Add timestamp
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // gmtime_r, not gmtime: gmtime returns a pointer to a shared static tm, so
        // two threads formatting a timestamp at once corrupt each other's -- even
        // when each owns its own FileLogger and is otherwise using it correctly, as
        // the "NOT thread-safe" note on the class intends.
        struct tm tm_buf{};
        gmtime_r(&time, &tm_buf);

        file_ << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
              << '.' << std::setfill('0') << std::setw(3) << ms.count()
              << " [INFO] " << msg << std::endl;
        
        if (auto_flush_) {
            file_.flush();
        }
    }
}

void FileLogger::logError(std::string_view msg) {
    if (file_.is_open()) {
        // Add timestamp
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // gmtime_r, not gmtime: gmtime returns a pointer to a shared static tm, so
        // two threads formatting a timestamp at once corrupt each other's -- even
        // when each owns its own FileLogger and is otherwise using it correctly, as
        // the "NOT thread-safe" note on the class intends.
        struct tm tm_buf{};
        gmtime_r(&time, &tm_buf);

        file_ << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
              << '.' << std::setfill('0') << std::setw(3) << ms.count()
              << " [ERROR] " << msg << std::endl;
        
        if (auto_flush_) {
            file_.flush();
        }
    }
}

void FileLogger::flush() {
    if (file_.is_open()) {
        file_.flush();
    }
}

void FileLogger::reopen() {
    if (file_.is_open()) {
        file_.close();
    }
    file_.open(filepath_, std::ios::app);
    if (file_.is_open()) {
        logMessage("Log file reopened (SIGHUP received)");
    } else {
        std::cerr << "FileLogger: Failed to reopen log file: " << filepath_ << std::endl;
    }
}

} // namespace caduvelox
