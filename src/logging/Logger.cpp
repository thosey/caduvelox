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

std::atomic<LogLevel> Logger::level_{LogLevel::Info};

void Logger::setLevel(LogLevel level) {
    level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::getLevel() {
    return level_.load(std::memory_order_relaxed);
}

void Logger::setGlobalLogger(Logger* ptr) {
    logger.store(ptr, std::memory_order_release);
}

Logger& Logger::getInstance() {
    auto* ptr = logger.load(std::memory_order_acquire);
    if (!ptr) {
        // No logger was installed, so fall back to the console.
        //
        // Deliberately leaked, and deliberately a raw pointer rather than a
        // function-static object: this reference is handed out to jobs that may
        // log during static destruction, and a static object would be destroyed
        // while they are still using it. Leaking one small object for the life of
        // the process is the cheaper trade. Sanitizer suppressions are not needed
        // -- LeakSanitizer sees it as still reachable.
        static caduvelox::ConsoleLogger* fallback_logger = new caduvelox::ConsoleLogger();
        return *fallback_logger;
    }
    return *ptr;
}

}
