#pragma once
#include <string>
#include <string_view>
#include <atomic>
#include <exception>

namespace caduvelox {

class Logger {
public:
    virtual ~Logger() = default;
    virtual void logMessage(std::string_view msg) = 0;
    virtual void logError(std::string_view msg) = 0;
    
    /**
     * Log the current exception with a context message.
     * Should be called from within a catch(...) block.
     * Combines the provided message with the exception details.
     */
    void logCurrentError(std::string_view context_msg);
    
    static void setGlobalLogger(Logger* ptr);
    static Logger& getInstance();
};


} // namespace caduvelox
