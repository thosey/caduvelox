#pragma once
#include <string>
#include <string_view>
#include <atomic>
#include <exception>

namespace caduvelox {

/**
 * How much of the running commentary to emit.
 *
 * Errors are never filtered. Info is lifecycle: a ring came up, a listener
 * bound, the server drained. Debug is the per-request and per-operation trace,
 * which is off by default -- a single small GET produces seven lines of it, and
 * they cost real time on the ring thread (see Logger::debug).
 */
enum class LogLevel { Error = 0, Info = 1, Debug = 2 };

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

    /**
     * Set the current verbosity. Defaults to Info: the per-request trace is off.
     *
     * Measured on one keep-alive connection, 20k requests, 7 trace lines each:
     * writing them through ConsoleLogger costs about 2.8 us of CPU per request
     * (~14%), and through AsyncLogger about 19 us, because that logger wakes its
     * consumer thread once per message. Building the strings costs 0.11 us, so
     * the saving is in not emitting them, not in how they are formatted -- which
     * is why this is a plain runtime check and not a macro or a lazy interface.
     */
    static void setLevel(LogLevel level);
    static LogLevel getLevel();

    /**
     * Is the per-request trace wanted? Cheap enough to call on the hot path.
     */
    static bool debugEnabled() {
        return level_.load(std::memory_order_relaxed) >= LogLevel::Debug;
    }

    /**
     * Emit a per-request/per-operation trace line. Does nothing unless the level
     * is Debug, so the call sites read the same as an unconditional log.
     */
    static void debug(std::string_view msg) {
        if (debugEnabled()) {
            getInstance().logMessage(msg);
        }
    }

    /**
     * Emit a lifecycle line: startup, shutdown, binding a listener. Always on at
     * the default level.
     */
    static void info(std::string_view msg) {
        if (level_.load(std::memory_order_relaxed) >= LogLevel::Info) {
            getInstance().logMessage(msg);
        }
    }

private:
    static std::atomic<LogLevel> level_;
};


} // namespace caduvelox
