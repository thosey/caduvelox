#include "caduvelox/logger/AsyncLogger.hpp"
#include "../ring_buffer/NotifyingRingBuffer.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>

namespace caduvelox {

enum class LogLevel : uint8_t { MESSAGE = 0, ERROR = 1 };

// Fixed-size log message for zero-malloc async logging
struct LogMessage {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    char message[1024]; // Fixed 1024-byte buffer
    size_t length;      // Actual message length

    // Constructor from string_view (zero-copy interface)
    LogMessage() = default;

    LogMessage(LogLevel lvl, std::string_view msg)
        : timestamp(std::chrono::system_clock::now()), level(lvl),
          length(std::min(msg.size(), sizeof(message) - 1)) {
        std::memcpy(message, msg.data(), length);
        message[length] = '\0'; // Null terminate for safety
    }

    // Get message as string_view
    std::string_view getMessage() const { return std::string_view(message, length); }
};

class AsyncLogger::Impl {
  public:
    Impl(std::unique_ptr<Logger> delegate)
        : fDelegate(std::move(delegate)), fRunning(true),
          fWorkerThread(&Impl::workerThreadFunc, this) {}

    ~Impl() {
        fRunning = false;
        fRingBuffer.shutdown(); // Wake the worker thread for clean shutdown
        if (fWorkerThread.joinable()) {
            fWorkerThread.join();
        }
    }

    void logMessage(std::string_view msg) { enqueueLogMessage(LogLevel::MESSAGE, msg); }

    void logError(std::string_view msg) { enqueueLogMessage(LogLevel::ERROR, msg); }

  private:
    void enqueueLogMessage(LogLevel level, std::string_view msg) {
        LogMessage logMsg(level, msg);

        if (!fRingBuffer.enqueue(std::move(logMsg))) {
            // Buffer full - fallback to synchronous logging with warning prefix
            std::string fallbackMsg = "[ASYNC_BUFFER_FULL] ";
            fallbackMsg += msg;
            
            switch (level) {
            case LogLevel::MESSAGE:
                fDelegate->logMessage(fallbackMsg);
                break;
            case LogLevel::ERROR:
                fDelegate->logError(fallbackMsg);
                break;
            }
        }
    }

    void workerThreadFunc() {
        LogMessage msg;
        while (fRunning.load(std::memory_order_relaxed)) {
            if (fRingBuffer.dequeue(msg)) {
                // Process the log message
                processLogMessage(msg);
            } else {
                // dequeue returned false - shutdown was signaled
                break;
            }
        }

        // Drain remaining messages on shutdown using non-blocking calls
        while (fRingBuffer.try_dequeue(msg)) {
            processLogMessage(msg);
        }
    }

    void processLogMessage(const LogMessage &msg) {
        switch (msg.level) {
        case LogLevel::MESSAGE:
            fDelegate->logMessage(msg.getMessage());
            break;
        case LogLevel::ERROR:
            fDelegate->logError(msg.getMessage());
            break;
        }
    }

    std::unique_ptr<Logger> fDelegate;
    NotifyingRingBuffer<LogMessage, 1024> fRingBuffer; // 1024 message buffer
    std::atomic<bool> fRunning;
    std::thread fWorkerThread;
};

AsyncLogger::AsyncLogger(std::unique_ptr<Logger> delegate)
    : fImpl(std::make_unique<AsyncLogger::Impl>(std::move(delegate))) {}

AsyncLogger::~AsyncLogger() = default;

void AsyncLogger::logMessage(std::string_view msg) { fImpl->logMessage(msg); }
void AsyncLogger::logError(std::string_view msg) { fImpl->logError(msg); }

} // namespace caduvelox
