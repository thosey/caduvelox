#include "caduvelox/logger/Logger.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"

#include <atomic>
#include <exception>
#include <string>

namespace {
    static std::atomic<caduvelox::Logger*> logger{nullptr};
}
namespace caduvelox {

void Logger::logCurrentError(std::string_view context_msg) {
    auto eptr = std::current_exception();
    std::string full_message = std::string(context_msg);
    
    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            full_message += ": ";
            full_message += e.what();
        } catch (...) {
            full_message += ": unknown exception type";
        }
    } else {
        full_message += ": no current exception";
    }
    
    logError(full_message);
}

void Logger::setGlobalLogger(Logger* ptr) {
    logger.store(ptr, std::memory_order_release);
}

Logger& Logger::getInstance() {
    auto* ptr = logger.load(std::memory_order_acquire);
    if (!ptr) {
        // Fallback: create a temporary console logger if none is set
        // Use a function-static variable to ensure it's initialized on first use
        static caduvelox::ConsoleLogger* fallback_logger = new caduvelox::ConsoleLogger();
        return *fallback_logger;
    }
    return *ptr;
}

}
