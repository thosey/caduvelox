#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/http/HttpResponse.hpp"
#include "caduvelox/http/HttpResponseWriter.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace caduvelox;
namespace fs = std::filesystem;

/**
 * Three ways a response said something untrue (review items L13, L15, L16).
 *
 *  - L16: status_code and status_text are independent public fields, so
 *    "res.status_code = 404" emitted "HTTP/1.1 404 OK". The static server example
 *    does exactly that.
 *  - L15: content-length was written on every response, including 204, which
 *    RFC 9110 section 8.6 forbids.
 *  - L13: a range whose offset is past end-of-file was answered 404, telling a
 *    client the resource does not exist when it does. RFC 9110 section 14.4 wants
 *    416 with Content-Range: bytes * / size, which also tells a resuming client
 *    the real length.
 *
 * L13 is reachable only through HTTPFileJob directly: nothing parses a Range
 * header, so the offset can only come from a caller constructing the job. That is
 * why it is tested at the job boundary rather than over a socket.
 */
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------
// L16 / L15 at the serializer, where every response passes through
// ---------------------------------------------------------------------------

TEST(ResponseStatusPhrase, ADirectlySetStatusCodeGetsTheRightPhrase) {
    HttpResponse res;
    res.status_code = 404;          // no setStatus(): the trap this is about
    std::string head;
    ASSERT_TRUE(build_response_head(res, 0, head));
    EXPECT_EQ(head.rfind("HTTP/1.1 404 Not Found\r\n", 0), 0u)
        << "status_text still said what the default said. Got:\n" << head;
}

TEST(ResponseStatusPhrase, AnExplicitPhraseIsKept) {
    HttpResponse res;
    res.setStatus(418, "I'm a teapot");
    std::string head;
    ASSERT_TRUE(build_response_head(res, 0, head));
    EXPECT_NE(head.find("418 I'm a teapot"), std::string::npos) << head;
}

TEST(ResponseStatusPhrase, AnUnknownCodeStillProducesSomething) {
    HttpResponse res;
    res.status_code = 599;
    std::string head;
    ASSERT_TRUE(build_response_head(res, 0, head));
    EXPECT_EQ(head.rfind("HTTP/1.1 599 ", 0), 0u) << head;
}

TEST(ResponseBodilessStatuses, NoContentCarriesNoContentLength) {
    for (int code : {204, 304}) {
        HttpResponse res;
        res.status_code = code;
        std::string head;
        ASSERT_TRUE(build_response_head(res, 0, head)) << code;
        EXPECT_EQ(lower(head).find("content-length"), std::string::npos)
            << code << " must not carry content-length. Got:\n" << head;
    }
}

// Guard: everything else still gets one.
TEST(ResponseBodilessStatuses, OrdinaryStatusesStillCarryContentLength) {
    for (int code : {200, 206, 404, 416, 500}) {
        HttpResponse res;
        res.status_code = code;
        std::string head;
        ASSERT_TRUE(build_response_head(res, 5, head)) << code;
        EXPECT_NE(lower(head).find("content-length: 5"), std::string::npos)
            << code << " should carry content-length. Got:\n" << head;
    }
}

// ---------------------------------------------------------------------------
// The same two, on the wire
// ---------------------------------------------------------------------------

class ResponseConformanceServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        port_ = BASE_PORT + counter_++;
        ASSERT_TRUE(job_server_.init(256));
        http_ = std::make_unique<SingleRingHttpServer>(job_server_);

        // A handler that sets status_code directly, as the example does.
        http_->addRoute("GET", "^/missing$", [](const HttpRequest&, HttpResponse& res) {
            res.status_code = 404;
            res.body = "nope\n";
        });
        http_->addRoute("DELETE", "^/thing$", [](const HttpRequest&, HttpResponse& res) {
            res.setStatus(204);
        });

        ASSERT_TRUE(http_->listen(port_, "127.0.0.1"));
        ring_ = std::thread([this] { job_server_.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void TearDown() override {
        if (http_) http_->stop();
        job_server_.stop();
        if (ring_.joinable()) ring_.join();
    }

    std::string request(const std::string& method, const std::string& target) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port_);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
        timeval tv{ .tv_sec = 3, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const std::string req =
            method + " " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, static_cast<size_t>(n));
        ::close(fd);
        return out;
    }

    static constexpr uint16_t BASE_PORT = 18800;
    static int counter_;
    uint16_t port_ = 0;
    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_;
    std::thread ring_;
};

int ResponseConformanceServerTest::counter_ = 0;

TEST_F(ResponseConformanceServerTest, AHandlerSettingStatusCodeDirectlyGetsTheRightPhrase) {
    const std::string r = request("GET", "/missing");
    EXPECT_NE(r.find("HTTP/1.1 404 Not Found"), std::string::npos)
        << "a handler that writes status_code without setStatus emitted the wrong "
           "reason phrase. Got:\n" << r;
}

TEST_F(ResponseConformanceServerTest, A204CarriesNeitherLengthNorBody) {
    const std::string r = request("DELETE", "/thing");
    ASSERT_NE(r.find("HTTP/1.1 204"), std::string::npos) << r;
    EXPECT_EQ(lower(r).find("content-length"), std::string::npos)
        << "RFC 9110 section 8.6 forbids content-length on a 204. Got:\n" << r;
    const size_t end = r.find("\r\n\r\n");
    ASSERT_NE(end, std::string::npos) << r;
    EXPECT_EQ(r.substr(end + 4), "") << "a 204 must have no body. Got:\n" << r;
}

// ---------------------------------------------------------------------------
// L13, at the job boundary
// ---------------------------------------------------------------------------

class RangePastEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(128));
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_), 0);
        dir_ = fs::temp_directory_path() / ("cadu_l13_" + std::to_string(::getpid()));
        fs::create_directories(dir_);
        path_ = (dir_ / "payload.txt").string();
        std::ofstream(path_) << "0123456789";   // ten bytes
    }

    void TearDown() override {
        if (sv_[0] >= 0) ::close(sv_[0]);
        if (sv_[1] >= 0) ::close(sv_[1]);
        fs::remove_all(dir_);
    }

    bool pumpUntilDone(int seconds = 3) {
        while (!done_) {
            struct __kernel_timespec ts{};
            ts.tv_sec = seconds;
            struct io_uring_cqe* cqe = nullptr;
            if (io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts) < 0) return false;
            auto* job = reinterpret_cast<IoJob*>(io_uring_cqe_get_data64(cqe));
            auto cleanup = job->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) (*cleanup)(job);
        }
        return true;
    }

    std::string clientBytes(int timeout_ms = 300) {
        std::string out;
        for (;;) {
            struct timeval tv{ .tv_sec = 0, .tv_usec = timeout_ms * 1000 };
            setsockopt(sv_[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[4096];
            ssize_t n = ::recv(sv_[1], buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
            timeout_ms = 50;
        }
        return out;
    }

    Server server_;
    int sv_[2] = {-1, -1};
    fs::path dir_;
    std::string path_;
    bool done_ = false;
};

TEST_F(RangePastEndTest, AnOffsetPastTheEndIsRangeNotSatisfiable) {
    auto* job = HTTPFileJob::createFromPool(
        sv_[0], path_, /*offset=*/100, /*length=*/10, HttpResponse{},
        [this](int, size_t) { done_ = true; },
        [this](int, int) { done_ = true; });
    ASSERT_NE(job, nullptr);

    job->start(server_);
    ASSERT_TRUE(pumpUntilDone());

    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 416"), std::string::npos)
        << "a range past the end of an existing file was reported as 404, which "
           "says the resource is absent. Got:\n" << sent;
    EXPECT_NE(lower(sent).find("content-range: bytes */10"), std::string::npos)
        << "416 should tell the client the real length. Got:\n" << sent;
}

// Guard: a range inside the file still works.
TEST_F(RangePastEndTest, ARangeInsideTheFileStillTransfers) {
    auto* job = HTTPFileJob::createFromPool(
        sv_[0], path_, /*offset=*/2, /*length=*/3, HttpResponse{},
        [this](int, size_t) { done_ = true; },
        [this](int, int) { done_ = true; });
    ASSERT_NE(job, nullptr);

    job->start(server_);
    ASSERT_TRUE(pumpUntilDone());

    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 206"), std::string::npos) << sent;
    EXPECT_NE(sent.find("234"), std::string::npos) << "expected bytes 2..4. Got:\n" << sent;
}

}  // namespace
