#include "caduvelox/logger/FileLogger.hpp"
#include <iostream>
#include <chrono>
#include <iomanip>

namespace caduvelox {

FileLogger::FileLogger(const std::string& filepath, bool auto_flush)
    : auto_flush_(auto_flush) {
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
        
        file_ << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
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
        
        file_ << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
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

} // namespace caduvelox
