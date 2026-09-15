#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "caduvelox/Server.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"

using namespace caduvelox;

/**
 * What a handler can make the server put on the wire (review item M7).
 *
 * HttpResponse::headers is a public map documented as holding lowercase keys,
 * but only setHeader() lowercases. Every step that fills in a default --
 * HttpRouter::fallback_to_default_headers(), sendResponse()'s Content-Length,
 * HTTPFileJob's Content-Length and Content-Type -- looks the key up in lowercase
 * and, not finding a mixed-case one, adds its own. Both are then written.
 *
 * Two Content-Length headers with different values make a response invalid
 * (RFC 9110 section 8.6), and a cache or proxy in front of this server may
 * believe a different one than the client does. Where they disagree about
 * where this response's body ends, one of them reads the start of the next
 * response out of it. That is the response-side twin of the request desync
 * closed earlier in the parser.
 *
 * These read raw bytes from a real server rather than parsing responses,
 * because a malformed response is exactly what a parser would hide. Each
 * request asks for Connection: close so the read can simply run to EOF, which
 * stays correct even when the framing on the wire is not.
 */
namespace {

class ResponseHeaderFramingTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        test_port_ = BASE_PORT + test_counter_++;

        dir_ = std::filesystem::temp_directory_path() /
               ("cadu_m7_" + std::to_string(::getpid()) + "_" + std::to_string(test_port_));
        std::filesystem::create_directories(dir_);
        {
            std::ofstream ofs(dir_ / "page.txt", std::ios::binary | std::ios::trunc);
            ofs << "file body";
        }

        ASSERT_TRUE(job_server_.init(256));
        http_server_ = std::make_unique<SingleRingHttpServer>(job_server_);

        // Mixed-case Content-Length that agrees with the body.
        route("/cl-mixed-agrees", [](const HttpRequest&, HttpResponse& res) {
            res.body = "hello";
            res.headers["Content-Length"] = "5";
        });
        // Mixed-case Content-Length that disagrees with the body.
        route("/cl-mixed-disagrees", [](const HttpRequest&, HttpResponse& res) {
            res.body = "hello";
            res.headers["Content-Length"] = "999";
        });
        // Correct case, wrong value: a single header, but not this body's length.
        route("/cl-lower-disagrees", [](const HttpRequest&, HttpResponse& res) {
            res.body = "hello";
            res.headers["content-length"] = "999";
        });
        // Not a framing header, but the router's default is added the same way.
        route("/ct-mixed", [](const HttpRequest&, HttpResponse& res) {
            res.body = "{}";
            res.headers["Content-Type"] = "application/json";
        });
        // A file response: HTTPFileJob fills in its own Content-Length and
        // Content-Type after the handler.
        route("/file-mixed", [this](const HttpRequest&, HttpResponse& res) {
            res.sendFile((dir_ / "page.txt").string());
            res.headers["Content-Length"] = "1";
            res.headers["Content-Type"] = "text/css";
        });
        // A header value that carries a line break.
        route("/value-crlf", [](const HttpRequest&, HttpResponse& res) {
            res.body = "hello";
            res.headers["x-note"] = "a\r\nContent-Length: 0";
        });
        // Status text that carries a line break.
        route("/status-crlf", [](const HttpRequest&, HttpResponse& res) {
            res.setStatus(200, "OK\r\nContent-Length: 0");
            res.body = "hello";
        });

        // A transfer coding this server will never apply.
        route("/te", [](const HttpRequest&, HttpResponse& res) {
            res.body = "hello";
            res.headers["Transfer-Encoding"] = "chunked";
        });
        // Two spellings of one field that disagree.
        route("/ct-conflict", [](const HttpRequest&, HttpResponse& res) {
            res.body = "hello";
            res.headers["Content-Type"] = "application/json";
            res.headers["content-type"] = "text/html";
        });
        // A status code that is not three digits.
        route("/bad-status", [](const HttpRequest&, HttpResponse& res) {
            res.status_code = 42;
            res.body = "hello";
        });

        // Guards.
        route("/plain", [](const HttpRequest&, HttpResponse& res) {
            res.setBody("hello");
        });
        route("/ct-lower", [](const HttpRequest&, HttpResponse& res) {
            res.body = "{}";
            res.headers["content-type"] = "application/json";
        });

        ASSERT_TRUE(http_server_->listen(test_port_, "127.0.0.1"));
        server_thread_ = std::thread([this]() { job_server_.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (http_server_) {
            http_server_->stop();
        }
        job_server_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    void route(const std::string& path, HttpHandler handler) {
        http_server_->addRoute("GET", "^" + path + "$", std::move(handler));
    }

    // Everything the server sends for one request, read to EOF.
    std::string fetchRaw(const std::string& path) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(test_port_);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        timeval tv{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const std::string req =
            "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        EXPECT_EQ(::send(fd, req.data(), req.size(), MSG_NOSIGNAL),
                  static_cast<ssize_t>(req.size()));

        std::string out;
        char buf[4096];
        for (;;) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) { out.append(buf, static_cast<size_t>(n)); continue; }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        ::close(fd);
        return out;
    }

    // The header block -- status line through the blank line -- split into lines.
    static std::vector<std::string> headerLines(const std::string& raw) {
        std::vector<std::string> lines;
        const size_t end = raw.find("\r\n\r\n");
        const std::string block = raw.substr(0, end == std::string::npos ? raw.size() : end);
        size_t pos = 0;
        for (;;) {
            size_t eol = block.find("\r\n", pos);
            lines.push_back(block.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
            if (eol == std::string::npos) break;
            pos = eol + 2;
        }
        return lines;
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Values of every header field named `name`, compared case-insensitively.
    static std::vector<std::string> fieldValues(const std::string& raw, const std::string& name) {
        std::vector<std::string> values;
        const auto lines = headerLines(raw);
        for (size_t i = 1; i < lines.size(); ++i) {  // line 0 is the status line
            const size_t colon = lines[i].find(':');
            if (colon == std::string::npos) continue;
            if (lower(lines[i].substr(0, colon)) != lower(name)) continue;
            size_t v = colon + 1;
            while (v < lines[i].size() && (lines[i][v] == ' ' || lines[i][v] == '\t')) ++v;
            values.push_back(lines[i].substr(v));
        }
        return values;
    }

    static std::string bodyOf(const std::string& raw) {
        const size_t end = raw.find("\r\n\r\n");
        return end == std::string::npos ? std::string() : raw.substr(end + 4);
    }

    static constexpr uint16_t BASE_PORT = 10000;
    static int test_counter_;

    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_server_;
    std::thread server_thread_;
    std::filesystem::path dir_;
    uint16_t test_port_ = 0;
};

int ResponseHeaderFramingTest::test_counter_ = 0;

// ---------------------------------------------------------------------------
// Duplicated fields
// ---------------------------------------------------------------------------

TEST_F(ResponseHeaderFramingTest, MixedCaseContentLengthIsNotDuplicated) {
    const std::string raw = fetchRaw("/cl-mixed-agrees");
    const auto cl = fieldValues(raw, "content-length");
    EXPECT_EQ(cl.size(), 1u)
        << "a handler's Content-Length missed the lowercase lookup, so a second "
           "one was added. Got:\n" << raw;
}

TEST_F(ResponseHeaderFramingTest, ConflictingContentLengthsCannotBothReachTheWire) {
    const std::string raw = fetchRaw("/cl-mixed-disagrees");
    const auto cl = fieldValues(raw, "content-length");
    ASSERT_EQ(cl.size(), 1u)
        << "two Content-Length headers with different values: a proxy and a "
           "client may each believe a different one. Got:\n" << raw;
    EXPECT_EQ(cl[0], std::to_string(bodyOf(raw).size()))
        << "the one Content-Length that went out does not describe the body";
}

TEST_F(ResponseHeaderFramingTest, ContentLengthAlwaysDescribesTheBodySent) {
    const std::string raw = fetchRaw("/cl-lower-disagrees");
    const auto cl = fieldValues(raw, "content-length");
    ASSERT_EQ(cl.size(), 1u) << raw;
    EXPECT_EQ(cl[0], std::to_string(bodyOf(raw).size()))
        << "a single, correctly-cased Content-Length still frames the message "
           "wrong if its value is not the body's length. Got:\n" << raw;
}

TEST_F(ResponseHeaderFramingTest, MixedCaseContentTypeIsNotDuplicated) {
    const std::string raw = fetchRaw("/ct-mixed");
    const auto ct = fieldValues(raw, "content-type");
    ASSERT_EQ(ct.size(), 1u)
        << "the router's default content-type was added alongside the "
           "handler's. Got:\n" << raw;
    EXPECT_EQ(ct[0], "application/json") << "the handler's value must win over the default";
}

TEST_F(ResponseHeaderFramingTest, FileResponseHeadersAreNotDuplicated) {
    const std::string raw = fetchRaw("/file-mixed");
    const auto cl = fieldValues(raw, "content-length");
    ASSERT_EQ(cl.size(), 1u) << raw;
    EXPECT_EQ(cl[0], "9") << "a file's length is the file's, not the handler's";
    EXPECT_EQ(bodyOf(raw), "file body");

    const auto ct = fieldValues(raw, "content-type");
    ASSERT_EQ(ct.size(), 1u) << raw;
    EXPECT_EQ(ct[0], "text/css") << "a handler-chosen type must win over the extension guess";
}

// ---------------------------------------------------------------------------
// Line breaks: the other way to put a second field on the wire
// ---------------------------------------------------------------------------

TEST_F(ResponseHeaderFramingTest, ALineBreakInAHeaderValueCannotAddAField) {
    const std::string raw = fetchRaw("/value-crlf");
    EXPECT_LE(fieldValues(raw, "content-length").size(), 1u)
        << "a CR LF inside a handler's header value started a new header line. Got:\n" << raw;
    for (const auto& line : headerLines(raw)) {
        EXPECT_NE(lower(line), "content-length: 0")
            << "the injected field reached the wire. Got:\n" << raw;
    }
}

TEST_F(ResponseHeaderFramingTest, ALineBreakInTheStatusTextCannotAddAField) {
    const std::string raw = fetchRaw("/status-crlf");
    EXPECT_LE(fieldValues(raw, "content-length").size(), 1u)
        << "a CR LF inside the status text started a new header line. Got:\n" << raw;
    for (const auto& line : headerLines(raw)) {
        EXPECT_NE(lower(line), "content-length: 0")
            << "the injected field reached the wire. Got:\n" << raw;
    }
}

// ---------------------------------------------------------------------------
// What happens to a response that cannot be written safely
//
// The handler's response is refused and a fixed 500 goes out instead. The
// alternatives were worse: dropping the offending field silently can drop a
// security header, and "repairing" a value means sending something the
// handler never asked for.
// ---------------------------------------------------------------------------

static void expectRefusedWith500(const std::string& raw) {
    EXPECT_NE(raw.find("HTTP/1.1 500"), std::string::npos) << "got:\n" << raw;
    const auto end = raw.find("\r\n\r\n");
    ASSERT_NE(end, std::string::npos) << raw;
    EXPECT_EQ(raw.substr(end + 4), "Internal Server Error") << raw;
}

TEST_F(ResponseHeaderFramingTest, ALineBreakInAHeaderValueIsRefusedWithA500) {
    expectRefusedWith500(fetchRaw("/value-crlf"));
}

TEST_F(ResponseHeaderFramingTest, ALineBreakInTheStatusTextIsRefusedWithA500) {
    expectRefusedWith500(fetchRaw("/status-crlf"));
}

TEST_F(ResponseHeaderFramingTest, ATransferEncodingFromAHandlerIsRefusedWithA500) {
    const std::string raw = fetchRaw("/te");
    expectRefusedWith500(raw);
    EXPECT_TRUE(fieldValues(raw, "transfer-encoding").empty())
        << "Transfer-Encoding alongside Content-Length contradicts it. Got:\n" << raw;
}

TEST_F(ResponseHeaderFramingTest, ConflictingSpellingsOfOneFieldAreRefusedWithA500) {
    const std::string raw = fetchRaw("/ct-conflict");
    expectRefusedWith500(raw);
    EXPECT_EQ(fieldValues(raw, "content-type").size(), 1u) << raw;
}

TEST_F(ResponseHeaderFramingTest, AnOutOfRangeStatusCodeIsRefusedWithA500) {
    expectRefusedWith500(fetchRaw("/bad-status"));
}

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------

TEST_F(ResponseHeaderFramingTest, SetBodyStillProducesOneCorrectContentLength) {
    const std::string raw = fetchRaw("/plain");
    ASSERT_NE(raw.find("HTTP/1.1 200"), std::string::npos) << raw;
    const auto cl = fieldValues(raw, "content-length");
    ASSERT_EQ(cl.size(), 1u) << raw;
    EXPECT_EQ(cl[0], "5");
    EXPECT_EQ(bodyOf(raw), "hello");
}

TEST_F(ResponseHeaderFramingTest, LowercaseDirectWritesStillWork) {
    const std::string raw = fetchRaw("/ct-lower");
    const auto ct = fieldValues(raw, "content-type");
    ASSERT_EQ(ct.size(), 1u) << raw;
    EXPECT_EQ(ct[0], "application/json");
}

TEST_F(ResponseHeaderFramingTest, FileResponsesStillCarryTheirOwnHeaders) {
    const std::string raw = fetchRaw("/file-mixed");
    ASSERT_NE(raw.find("HTTP/1.1 200"), std::string::npos) << raw;
    EXPECT_EQ(fieldValues(raw, "access-control-allow-origin").size(), 1u) << raw;
}

}  // namespace
