#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cctype>
#include <chrono>
#include <string>
#include <thread>

using namespace caduvelox;

/**
 * What a rejected request gets told (review item M15).
 *
 * Every parse failure was answered by logging a line and closing the socket.
 * Nothing was ever sent. Measured before the change: "GET /files/a b.txt
 * HTTP/1.1" -- a literal space, which is what a client sends when it fails to
 * encode -- produced an empty reply and a closed connection.
 *
 * That matters more since the parser became strict (a field line without a
 * colon, whitespace before a colon, an obs-fold continuation, a bare LF in a
 * value, a non-token field name, a bad version, a non-numeric or contradictory
 * Content-Length, any Transfer-Encoding). Each of those is a deliberate
 * rejection, and every one of them looked to the client exactly like a network
 * fault. RFC 9112 section 3 says to answer 400 first.
 *
 * The response has to be the last thing on the connection: once framing is
 * untrustworthy, nothing further may be parsed and anything already buffered is
 * dropped. So the 400 carries "connection: close", and a request pipelined
 * behind a malformed one must not be answered -- which is what
 * ARequestBehindAMalformedOneIsNotAnswered checks.
 */
namespace {

class MalformedRequestTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        port_ = BASE_PORT + counter_++;
        ASSERT_TRUE(job_server_.init(256));
        http_ = std::make_unique<SingleRingHttpServer>(job_server_);
        http_->addRoute("GET", "^/ok$", [](const HttpRequest&, HttpResponse& res) {
            res.setBody("fine");
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

    struct Reply {
        int status = 0;
        int responses = 0;
        bool closed = false;
        std::string raw;
    };

    // Send `request` verbatim and read until the server hangs up or goes quiet.
    Reply send(const std::string& request) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port_);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
        timeval tv{ .tv_sec = 3, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        size_t off = 0;
        while (off < request.size()) {
            ssize_t n = ::send(fd, request.data() + off, request.size() - off, MSG_NOSIGNAL);
            if (n <= 0) break;   // server may close mid-send on an oversize request
            off += static_cast<size_t>(n);
        }

        Reply out;
        char buf[8192];
        for (;;) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) { out.raw.append(buf, static_cast<size_t>(n)); continue; }
            if (n == 0) { out.closed = true; }
            break;
        }
        ::close(fd);

        if (out.raw.size() > 12) out.status = std::atoi(out.raw.substr(9, 3).c_str());
        size_t pos = 0;
        while ((pos = out.raw.find("HTTP/1.1 ", pos)) != std::string::npos) { ++out.responses; pos += 9; }
        return out;
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static constexpr uint16_t BASE_PORT = 18500;
    static int counter_;

    uint16_t port_ = 0;
    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_;
    std::thread ring_;
};

int MalformedRequestTest::counter_ = 0;

// ---------------------------------------------------------------------------
// Every parser rejection gets a 400
// ---------------------------------------------------------------------------

TEST_F(MalformedRequestTest, EachKindOfMalformedRequestIsAnswered) {
    struct Case { const char* what; std::string request; };
    const Case cases[] = {
        {"unencoded space in the target",
         "GET /files/a b.txt HTTP/1.1\r\nHost: x\r\n\r\n"},
        {"field line with no colon",
         "GET /ok HTTP/1.1\r\nHost: x\r\nBadHeaderLine\r\n\r\n"},
        {"whitespace before the colon",
         "GET /ok HTTP/1.1\r\nHost : x\r\n\r\n"},
        {"obs-fold continuation line",
         "GET /ok HTTP/1.1\r\nHost: x\r\n  folded\r\n\r\n"},
        {"non-token field name",
         "GET /ok HTTP/1.1\r\nHo st: x\r\n\r\n"},
        {"bad HTTP version",
         "GET /ok HTTP/1.x\r\nHost: x\r\n\r\n"},
        {"non-numeric Content-Length",
         "POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n"},
        {"contradictory Content-Length",
         "POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello"},
        {"Transfer-Encoding, which this server applies none of",
         "POST /ok HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\nContent-Length: 0\r\n\r\n"},
    };

    for (const auto& c : cases) {
        const Reply r = send(c.request);
        EXPECT_EQ(r.status, 400) << c.what << " got:\n" << r.raw;
        EXPECT_NE(lower(r.raw).find("connection: close"), std::string::npos)
            << c.what << ": the framing is untrustworthy, so the response must say it "
               "is the last one. Got:\n" << r.raw;
        EXPECT_TRUE(r.closed) << c.what << ": the connection should be closed afterwards";
    }
}

/**
 * The important guard. After a malformed request the byte stream cannot be
 * trusted to be at a request boundary, so whatever follows must not be treated
 * as a request -- an attacker chooses what that is.
 */
TEST_F(MalformedRequestTest, ARequestBehindAMalformedOneIsNotAnswered) {
    const Reply r = send("GET /ok HTTP/1.1\r\nHost: x\r\nBadHeaderLine\r\n\r\n"
                         "GET /ok HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_EQ(r.status, 400) << r.raw;
    EXPECT_EQ(r.responses, 1)
        << "only the 400 may be sent; the request behind it must be discarded. Got:\n" << r.raw;
    EXPECT_EQ(r.raw.find("fine"), std::string::npos)
        << "the pipelined request was routed and answered";
    EXPECT_TRUE(r.closed);
}

// ---------------------------------------------------------------------------
// Too large, which was the other silent close
// ---------------------------------------------------------------------------

TEST_F(MalformedRequestTest, OversizeHeadersAreAnsweredWith431) {
    // Header lines and no terminator, past the connection's 1 MiB limit. The
    // parser keeps saying "incomplete" until the buffer limit trips.
    std::string request = "GET /ok HTTP/1.1\r\nHost: x\r\n";
    const std::string line = "X-Pad: " + std::string(1000, 'a') + "\r\n";
    while (request.size() < 1100u * 1024u) request += line;

    const Reply r = send(request);
    EXPECT_EQ(r.status, 431)
        << "an over-long header section is 431 Request Header Fields Too Large, "
           "not a silent close. Got:\n" << r.raw.substr(0, 200);
    EXPECT_TRUE(r.closed);
}

TEST_F(MalformedRequestTest, AnOversizeBodyIsAnsweredWith413) {
    // Headers complete, then a body that pushes the buffer past the limit.
    std::string request = "POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n";
    request += std::string(1100u * 1024u, 'b');

    const Reply r = send(request);
    EXPECT_EQ(r.status, 413)
        << "a body over the limit is 413 Content Too Large. Got:\n" << r.raw.substr(0, 200);
    EXPECT_TRUE(r.closed);
}

// ---------------------------------------------------------------------------
// Guard: well-formed traffic is untouched
// ---------------------------------------------------------------------------

TEST_F(MalformedRequestTest, AValidRequestStillSucceedsAndKeepsTheConnection) {
    const Reply r = send("GET /ok HTTP/1.1\r\nHost: x\r\n\r\n"
                         "GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.responses, 2) << "both pipelined requests should be answered. Got:\n" << r.raw;
}

}  // namespace
