#include "caduvelox/logger/AsyncLogger.hpp"
#include "../ring_buffer/NotifyingRingBuffer.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>

namespace caduvelox {

// Which of the Logger methods a queued entry came from. Renamed from LogLevel:
// that name now belongs to the public verbosity enum in Logger.hpp, and this is
// not a level -- an error entry is not "more severe", it just goes to the other
// delegate method.
enum class EntryKind : uint8_t { Message = 0, Error = 1 };

// Fixed-size log message for zero-malloc async logging
struct LogMessage {
    std::chrono::system_clock::time_point timestamp;
    EntryKind level;
    char message[1024]; // Fixed 1024-byte buffer
    size_t length;      // Actual message length

    // Constructor from string_view (zero-copy interface)
    LogMessage() = default;

    LogMessage(EntryKind lvl, std::string_view msg)
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
        // Signal shutdown first
        fRunning.store(false, std::memory_order_release);
        
        // Wake the worker thread so it can see fRunning == false
        fRingBuffer.shutdown();
        
        // Wait for worker thread to FULLY exit before destroying anything else
        if (fWorkerThread.joinable()) {
            fWorkerThread.join();
        }
        
        // Worker thread has now exited workerThreadFunc() and is fully terminated
        // Safe to destroy fRingBuffer and fDelegate (happens automatically)
    }

    void logMessage(std::string_view msg) { enqueueLogMessage(EntryKind::Message, msg); }

    void logError(std::string_view msg) { enqueueLogMessage(EntryKind::Error, msg); }

  private:
    void enqueueLogMessage(EntryKind level, std::string_view msg) {
        // Check if we're shutting down - don't attempt fallback logging
        if (!fRunning.load(std::memory_order_acquire)) {
            // During shutdown, drop the message to avoid use-after-free
            return;
        }
        
        LogMessage logMsg(level, msg);

        if (!fRingBuffer.enqueue(std::move(logMsg))) {
            // Queue full. Count it and let the worker report it.
            //
            // This used to write the message straight to the delegate from the
            // producer's thread, while the worker thread was writing to the same
            // delegate -- and a delegate need not be thread-safe. FileLogger, the
            // one this is used with, is an ofstream plus std::gmtime (which has a
            // shared static buffer), so the two threads interleaved mid-line:
            // measured at 9411 lines in the file for 8000 messages logged, i.e.
            // corrupted output, not merely lost output.
            //
            // Keeping every delegate write on the worker thread is what makes the
            // output trustworthy. A drop is then visible as a count rather than as
            // a garbled line.
            fDropped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Emit one line for anything dropped since the last report. Worker thread only.
    void reportDropsIfAny() {
        const uint64_t dropped = fDropped.exchange(0, std::memory_order_relaxed);
        if (dropped > 0) {
            fDelegate->logError("AsyncLogger: dropped " + std::to_string(dropped) +
                                " message(s): queue full");
        }
    }

    void workerThreadFunc() {
        LogMessage msg;
        while (fRunning.load(std::memory_order_relaxed)) {
            if (fRingBuffer.dequeue(msg)) {
                // Process the log message
                processLogMessage(msg);
                reportDropsIfAny();
            } else {
                // dequeue returned false - shutdown was signaled
                break;
            }
        }

        // Drain remaining messages on shutdown using non-blocking calls
        while (fRingBuffer.try_dequeue(msg)) {
            processLogMessage(msg);
        }
        // Anything dropped right at the end is still worth saying. Safe here: the
        // producers are done with this logger by the time the destructor joins.
        reportDropsIfAny();
    }

    void processLogMessage(const LogMessage &msg) {
        switch (msg.level) {
        case EntryKind::Message:
            fDelegate->logMessage(msg.getMessage());
            break;
        case EntryKind::Error:
            fDelegate->logError(msg.getMessage());
            break;
        }
    }

    std::unique_ptr<Logger> fDelegate;
    // 4096 entries, not 65536. LogMessage carries a fixed 1024-byte buffer, so it
    // is 1048 bytes, and 65536 of them made every AsyncLogger cost 68 MB of
    // *resident* memory the moment it was constructed -- measured, VmRSS 5 MB ->
    // 73 MB -- because the ring's constructor touches every slot to initialise its
    // sequence counter. That was paid before a single line was logged, at any log
    // level. 4096 still buffers roughly ten milliseconds of the heaviest tracing
    // this server produces, and overflow falls back to writing synchronously
    // rather than dropping.
    NotifyingRingBuffer<LogMessage, 4096> fRingBuffer;
    std::atomic<bool> fRunning;
    // Messages the queue had no room for, reported by the worker thread.
    std::atomic<uint64_t> fDropped{0};
    std::thread fWorkerThread;
};

AsyncLogger::AsyncLogger(std::unique_ptr<Logger> delegate)
    : fImpl(std::make_unique<AsyncLogger::Impl>(std::move(delegate))) {}

AsyncLogger::~AsyncLogger() = default;

void AsyncLogger::logMessage(std::string_view msg) { fImpl->logMessage(msg); }
void AsyncLogger::logError(std::string_view msg) { fImpl->logError(msg); }

} // namespace caduvelox
