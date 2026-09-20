#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace caduvelox;

/**
 * Log levels, and what the per-request trace costs (review item M1).
 *
 * Every log line used to be emitted unconditionally. A single small GET produces
 * seven of them, and ConsoleLogger flushes on each one (std::endl), on the ring
 * thread.
 *
 * Measured on one keep-alive connection over 20k requests, as process CPU per
 * request: 17.3 us with no logging, 20.1 us through ConsoleLogger, 36.3 us
 * through AsyncLogger -- which wakes its consumer once per message (filed as
 * review item M14). Building the message strings accounts for only 0.11 us of
 * that, which is why the fix is a plain level check rather than a macro or a
 * lazily-formatted interface: there is nothing to gain by deferring the
 * formatting, only by not emitting the line.
 */
namespace {

class CapturingLogger : public Logger {
public:
    void logMessage(std::string_view msg) override {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.emplace_back(msg);
    }
    void logError(std::string_view msg) override {
        std::lock_guard<std::mutex> lock(mutex_);
        errors_.emplace_back(msg);
    }

    std::vector<std::string> messages() {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }
    std::vector<std::string> errors() {
        std::lock_guard<std::mutex> lock(mutex_);
        return errors_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        errors_.clear();
    }

private:
    std::mutex mutex_;
    std::vector<std::string> messages_;
    std::vector<std::string> errors_;
};

bool containsSubstring(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Restores the global logger and level, so this file cannot disturb the rest of
// the binary -- both are process-wide.
class LoggerLevelTest : public ::testing::Test {
protected:
    void SetUp() override {
        previous_level_ = Logger::getLevel();
        Logger::setGlobalLogger(&capture_);
    }
    void TearDown() override {
        Logger::setLevel(previous_level_);
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
    }

    CapturingLogger capture_;
    LogLevel previous_level_ = LogLevel::Info;
};

// ---------------------------------------------------------------------------
// The level itself
// ---------------------------------------------------------------------------

TEST_F(LoggerLevelTest, InfoIsTheDefaultAndDebugIsOff) {
    EXPECT_EQ(Logger::getLevel(), LogLevel::Info);
    EXPECT_FALSE(Logger::debugEnabled());

    Logger::debug("trace line");
    Logger::info("lifecycle line");

    EXPECT_FALSE(containsSubstring(capture_.messages(), "trace line"));
    EXPECT_TRUE(containsSubstring(capture_.messages(), "lifecycle line"));
}

TEST_F(LoggerLevelTest, DebugLevelLetsTheTraceThrough) {
    Logger::setLevel(LogLevel::Debug);
    EXPECT_TRUE(Logger::debugEnabled());

    Logger::debug("trace line");
    Logger::info("lifecycle line");

    EXPECT_TRUE(containsSubstring(capture_.messages(), "trace line"));
    EXPECT_TRUE(containsSubstring(capture_.messages(), "lifecycle line"));
}

TEST_F(LoggerLevelTest, ErrorLevelSilencesEverythingButErrors) {
    Logger::setLevel(LogLevel::Error);

    Logger::debug("trace line");
    Logger::info("lifecycle line");
    Logger::getInstance().logError("error line");

    EXPECT_TRUE(capture_.messages().empty())
        << "nothing but errors should be emitted at the Error level";
    EXPECT_TRUE(containsSubstring(capture_.errors(), "error line"))
        << "errors are never filtered";
}

// ---------------------------------------------------------------------------
// What a real request emits
// ---------------------------------------------------------------------------

class RequestTraceTest : public LoggerLevelTest {
protected:
    // Serve one request and return what was logged while doing it.
    void serveOneRequest(uint16_t port) {
        Server job_server;
        ASSERT_TRUE(job_server.init(256));
        SingleRingHttpServer http(job_server);
        http.addRoute("GET", "^/trace$", [](const HttpRequest&, HttpResponse& res) {
            res.setBody("ok");
        });
        ASSERT_TRUE(http.listen(port, "127.0.0.1"));

        std::thread ring([&] { job_server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        capture_.clear();

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        timeval tv{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const std::string req =
            "GET /trace HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        ASSERT_EQ(::send(fd, req.data(), req.size(), MSG_NOSIGNAL),
                  static_cast<ssize_t>(req.size()));

        std::string got;
        char buf[4096];
        for (;;) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);

        http.stop();
        job_server.stop();
        ring.join();

        ASSERT_NE(got.find("HTTP/1.1 200"), std::string::npos) << "got:\n" << got;
    }
};

TEST_F(RequestTraceTest, TheDefaultLevelEmitsNoPerRequestTrace) {
    serveOneRequest(10300);

    const auto lines = capture_.messages();
    for (const char* trace : {"HttpConnectionJob: Processing",
                              "HttpConnectionJob: Response generated",
                              "HttpConnectionJob: Sending response",
                              "HttpConnectionJob: Response sent",
                              "HttpConnectionJob: Created for fd="}) {
        EXPECT_FALSE(containsSubstring(lines, trace))
            << "per-request trace line still emitted at the default level: " << trace;
    }
}

TEST_F(RequestTraceTest, AccessLoggingSurvivesTheDefaultLevel) {
    serveOneRequest(10301);

    const auto lines = capture_.messages();
    EXPECT_TRUE(containsSubstring(lines, "[ACCESS]"))
        << "access logging is the point of an access log; it stays on by default";
}

TEST_F(RequestTraceTest, DebugLevelRestoresTheFullTrace) {
    Logger::setLevel(LogLevel::Debug);
    serveOneRequest(10302);

    const auto lines = capture_.messages();
    EXPECT_TRUE(containsSubstring(lines, "HttpConnectionJob: Processing"))
        << "turning Debug on has to bring the trace back";
    EXPECT_TRUE(containsSubstring(lines, "HttpConnectionJob: Response sent"));
}

}  // namespace
