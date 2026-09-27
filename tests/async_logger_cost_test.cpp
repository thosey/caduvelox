#include <gtest/gtest.h>
#include "caduvelox/logger/AsyncLogger.hpp"
#include "caduvelox/logger/FileLogger.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace caduvelox;
namespace fs = std::filesystem;

/**
 * What AsyncLogger costs, and that batching its wake-ups did not lose anything
 * (review item M14).
 *
 * Measured before the change, at seven trace lines per HTTP request:
 *
 *   - +19.0 us of CPU per request, against +2.8 us for logging synchronously to
 *     the console. enqueue() signalled the consumer for every message, and the
 *     consumer drains in well under a microsecond, so it was parked again by the
 *     time the next message arrived: a futex wake and a futex wait per line.
 *   - 68 MB of *resident* memory per instance the moment one was constructed
 *     (VmRSS 5 MB -> 73 MB), because LogMessage carries a fixed 1024-byte buffer,
 *     the queue held 65536 of them, and the ring's constructor touches every slot
 *     to initialise its sequence counter. Paid before a single line was logged,
 *     at any log level.
 *
 * Producers now signal only once a batch has built up, and the consumer parks for
 * a bounded time so an item below the watermark is still collected promptly. The
 * risk in that trade is a stranded or lost message, which is what the two
 * behavioural tests below are for -- they are the reason to trust the change, not
 * the numbers.
 */
namespace {

long readVmRssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::stol(line.substr(line.find_first_of("0123456789")));
        }
    }
    return -1;
}

size_t countLines(const fs::path& p) {
    std::ifstream f(p);
    size_t n = 0;
    std::string line;
    while (std::getline(f, line)) ++n;
    return n;
}

class AsyncLoggerCostTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("cadu_m14_" + std::to_string(::getpid()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        fs::remove_all(dir_);
    }
    fs::path dir_;
};

TEST_F(AsyncLoggerCostTest, ConstructingOneDoesNotCostTensOfMegabytes) {
    const long before = readVmRssKb();
    ASSERT_GT(before, 0) << "could not read VmRSS";

    auto sink = std::make_unique<FileLogger>((dir_ / "mem.log").string(), true);
    auto logger = std::make_unique<AsyncLogger>(std::move(sink));
    logger->logMessage("touch the queue");

    const long after = readVmRssKb();
    const long grew = after - before;

    // Was ~68 MB. The bound is generous so a sanitizer build's own overhead does
    // not fail it, while still catching a return to a 65536-deep queue of
    // kilobyte slots.
    EXPECT_LT(grew, 24 * 1024)
        << "constructing an AsyncLogger grew RSS by " << grew << " kB";
}

/**
 * The correctness question the batching change raises: with producers no longer
 * signalling every message, does anything get left behind?
 */
TEST_F(AsyncLoggerCostTest, EveryMessageFromEveryProducerArrives) {
    const fs::path log = dir_ / "all.log";
    constexpr int threads = 4;
    // Fits the queue, so enqueue cannot fail however slow the consumer is: this
    // test is about the batching not losing anything, not about overflow.
    constexpr int per_thread = 500;

    {
        auto sink = std::make_unique<FileLogger>(log.string(), true);
        AsyncLogger logger(std::move(sink));

        std::vector<std::thread> producers;
        for (int t = 0; t < threads; ++t) {
            producers.emplace_back([&logger, t] {
                for (int i = 0; i < per_thread; ++i) {
                    logger.logMessage("producer " + std::to_string(t) + " message " +
                                      std::to_string(i));
                }
            });
        }
        for (auto& p : producers) p.join();
        // The destructor signals shutdown, joins the worker and drains what is left.
    }

    EXPECT_EQ(countLines(log), static_cast<size_t>(threads * per_thread))
        << "messages were lost between the producers and the sink";
}

/**
 * A single message does not reach the batching watermark, so only the consumer's
 * bounded park collects it. If that park were unbounded -- as it was, using an
 * atomic wait with no timeout -- this line would sit in the queue until some
 * later message happened to arrive.
 */
TEST_F(AsyncLoggerCostTest, ALoneMessageIsNotStrandedBelowTheWatermark) {
    const fs::path log = dir_ / "lone.log";
    auto sink = std::make_unique<FileLogger>(log.string(), true);
    AsyncLogger logger(std::move(sink));

    logger.logMessage("the only line");

    // FileLogger flushes every line, so the file shows it as soon as the consumer
    // has written it. Generous bound: the park is a millisecond.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool seen = false;
    while (!seen && std::chrono::steady_clock::now() < deadline) {
        if (countLines(log) >= 1) { seen = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    EXPECT_TRUE(seen) << "a single message never reached the sink";
}

/**
 * More messages than the queue can hold. Drops are expected -- the point is that
 * the log stays readable and says how many were dropped.
 *
 * Before, a producer whose enqueue failed wrote to the delegate itself, while the
 * worker thread was writing to the same delegate. FileLogger is an ofstream plus
 * std::gmtime, neither of which is thread-safe, and the writes interleaved
 * mid-line: 8000 messages produced 9411 lines. Every line here must be either a
 * message or a drop report, and delivered plus dropped must equal what was sent.
 */
TEST_F(AsyncLoggerCostTest, OverflowIsReportedRatherThanCorruptingTheLog) {
    const fs::path log = dir_ / "overflow.log";
    constexpr int threads = 4;
    constexpr int per_thread = 5000;   // 20000 into a 4096-deep queue

    {
        auto sink = std::make_unique<FileLogger>(log.string(), true);
        AsyncLogger logger(std::move(sink));

        std::vector<std::thread> producers;
        for (int t = 0; t < threads; ++t) {
            producers.emplace_back([&logger, t] {
                for (int i = 0; i < per_thread; ++i) {
                    logger.logMessage("producer " + std::to_string(t) + " message " +
                                      std::to_string(i));
                }
            });
        }
        for (auto& p : producers) p.join();
    }

    size_t delivered = 0;
    unsigned long long dropped = 0;
    size_t unrecognised = 0;
    std::ifstream f(log);
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("producer ") != std::string::npos &&
            line.find(" message ") != std::string::npos) {
            ++delivered;
        } else if (const size_t at = line.find("dropped "); at != std::string::npos) {
            dropped += std::strtoull(line.c_str() + at + 8, nullptr, 10);
        } else {
            ++unrecognised;
            if (unrecognised <= 3) ADD_FAILURE() << "garbled line: " << line;
        }
    }

    EXPECT_EQ(unrecognised, 0u)
        << "the log contains lines that are neither a message nor a drop report, "
           "which is what concurrent writes to one ofstream look like";
    EXPECT_EQ(delivered + dropped, static_cast<unsigned long long>(threads * per_thread))
        << "delivered " << delivered << " + dropped " << dropped
        << " does not account for everything logged";
    EXPECT_GT(dropped, 0u) << "this load is meant to overflow; if it did not, the "
                              "test is no longer exercising the overflow path";
}

}  // namespace
