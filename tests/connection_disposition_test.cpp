#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <cctype>
#include <cstdlib>
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
 * Whether a connection persists, and whether the response says so
 * (review items M8 and L14).
 *
 * Connection is a comma-separated list of options (RFC 9110 section 7.6.1).
 * The server compared the whole value against exactly "close" or
 * "keep-alive", so "Connection: close, TE" matched neither and fell through to
 * the HTTP/1.1 default. RFC 9112 section 9.6 is explicit about what "close"
 * obliges: the server closes after this response and does not process further
 * requests on the connection -- but a pipelined request sent after it was
 * answered anyway. A repeated Connection field lost all but its last line too,
 * so "close" on an earlier line vanished.
 *
 * Separately, a body response never advertised the decision. The connection
 * closed correctly on "Connection: close", but the response carried no
 * "connection: close", so an HTTP/1.1 client was told the connection
 * persisted. File responses already said it; body responses did not.
 *
 * These drive a real server over a real socket and read raw bytes, because the
 * behaviour under test is what happens to the connection after the response.
 */
namespace {

class ConnectionDispositionTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        test_port_ = BASE_PORT + test_counter_++;

        dir_ = std::filesystem::temp_directory_path() /
               ("cadu_m8_" + std::to_string(::getpid()) + "_" + std::to_string(test_port_));
        std::filesystem::create_directories(dir_);
        {
            std::ofstream ofs(dir_ / "page.txt", std::ios::binary | std::ios::trunc);
            ofs << "file body";
        }

        ASSERT_TRUE(job_server_.init(256));
        http_server_ = std::make_unique<SingleRingHttpServer>(job_server_);

        http_server_->addRoute("GET", "^/body$", [](const HttpRequest&, HttpResponse& res) {
            res.setBody("hello");
        });
        http_server_->addRoute("GET", "^/file$", [this](const HttpRequest&, HttpResponse& res) {
            res.sendFile((dir_ / "page.txt").string());
        });
        // A handler asking for the connection to be closed after its response.
        http_server_->addRoute("GET", "^/handler-close$", [](const HttpRequest&, HttpResponse& res) {
            res.setBody("bye");
            res.setHeader("Connection", "close");
        });
        // A handler claiming the connection persists, whatever the client asked.
        http_server_->addRoute("GET", "^/handler-keepalive$", [](const HttpRequest&, HttpResponse& res) {
            res.setBody("hello");
            res.setHeader("Connection", "keep-alive");
        });

        ASSERT_TRUE(http_server_->listen(test_port_, "127.0.0.1"));
        server_thread_ = std::thread([this]() { job_server_.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (client_fd_ >= 0) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
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

    int connectClient() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(test_port_);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        client_fd_ = fd;
        return fd;
    }

    static bool sendAll(int fd, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    static std::string get(const std::string& path, const std::string& version,
                           const std::vector<std::string>& extra_headers) {
        std::string req = "GET " + path + " " + version + "\r\nHost: localhost\r\n";
        for (const auto& h : extra_headers) req += h + "\r\n";
        return req + "\r\n";
    }

    // Complete responses (headers plus Content-Length bytes) at the front of buf.
    static std::vector<std::string> completeResponses(const std::string& buf) {
        std::vector<std::string> out;
        size_t pos = 0;
        for (;;) {
            const size_t header_end = buf.find("\r\n\r\n", pos);
            if (header_end == std::string::npos) break;
            const std::string head = lower(buf.substr(pos, header_end + 4 - pos));
            size_t body = 0;
            const size_t key = head.find("content-length:");
            if (key != std::string::npos) {
                body = static_cast<size_t>(std::strtoul(head.c_str() + key + 15, nullptr, 10));
            }
            const size_t total = (header_end + 4 - pos) + body;
            if (buf.size() - pos < total) break;
            out.push_back(buf.substr(pos, total));
            pos += total;
        }
        return out;
    }

    struct Outcome {
        std::vector<std::string> responses;
        bool closed = false;  // the server hung up
    };

    // Read until `want` complete responses have arrived and then `linger_ms` more,
    // or until the server closes. Distinguishes "closed" from "quiet but open".
    static Outcome readOutcome(int fd, size_t want, int linger_ms = 400) {
        Outcome o;
        std::string buf;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::chrono::steady_clock::time_point quiet_until{};
        for (;;) {
            o.responses = completeResponses(buf);
            const auto now = std::chrono::steady_clock::now();
            if (o.responses.size() >= want) {
                if (quiet_until == std::chrono::steady_clock::time_point{}) {
                    quiet_until = now + std::chrono::milliseconds(linger_ms);
                }
                if (now >= quiet_until) break;
            }
            if (now >= deadline) break;

            timeval tv{ .tv_sec = 0, .tv_usec = 50 * 1000 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char tmp[16384];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0) { buf.append(tmp, static_cast<size_t>(n)); continue; }
            if (n == 0) { o.closed = true; break; }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            o.closed = true;  // ECONNRESET and friends
            break;
        }
        o.responses = completeResponses(buf);
        return o;
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // The value of the response's connection field, or "" if it has none.
    static std::string connectionField(const std::string& response) {
        const std::string head = lower(response.substr(0, response.find("\r\n\r\n")));
        const size_t key = head.find("\r\nconnection:");
        if (key == std::string::npos) return "";
        size_t v = key + 13;
        while (v < head.size() && (head[v] == ' ' || head[v] == '\t')) ++v;
        return head.substr(v, head.find("\r\n", v) - v);
    }

    static std::string dump(const Outcome& o) {
        std::string s = o.closed ? "[server closed]\n" : "[connection open]\n";
        for (const auto& r : o.responses) s += r + "\n----\n";
        return s;
    }

    static constexpr uint16_t BASE_PORT = 10100;
    static int test_counter_;

    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_server_;
    std::thread server_thread_;
    std::filesystem::path dir_;
    uint16_t test_port_ = 0;
    int client_fd_ = -1;
};

int ConnectionDispositionTest::test_counter_ = 0;

// ---------------------------------------------------------------------------
// M8: "close" is an option in a list, not the whole value
// ---------------------------------------------------------------------------

TEST_F(ConnectionDispositionTest, CloseAmongOtherOptionsStopsThePipeline) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.1", {"Connection: close, TE", "TE: trailers"}) +
                            get("/body", "HTTP/1.1", {})));
    const Outcome o = readOutcome(fd, 1);

    EXPECT_EQ(o.responses.size(), 1u)
        << "the client said close; a request pipelined after that must not be "
           "processed. Got:\n" << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, CloseLaterInTheListCountsToo) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.1", {"Connection: TE ,  Close"}) +
                            get("/body", "HTTP/1.1", {})));
    const Outcome o = readOutcome(fd, 1);

    EXPECT_EQ(o.responses.size(), 1u) << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, CloseOnAnEarlierRepeatedLineIsNotLost) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.1", {"Connection: close", "Connection: keep-alive"}) +
                            get("/body", "HTTP/1.1", {})));
    const Outcome o = readOutcome(fd, 1);

    EXPECT_EQ(o.responses.size(), 1u)
        << "a repeated list field is one list; keeping only its last line "
           "dropped the close. Got:\n" << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, CloseWinsOverKeepAliveInTheSameList) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.1", {"Connection: keep-alive, close"}) +
                            get("/body", "HTTP/1.1", {})));
    const Outcome o = readOutcome(fd, 1);

    EXPECT_EQ(o.responses.size(), 1u) << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, Http10KeepAliveAmongOtherOptionsPersists) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.0", {"Connection: Keep-Alive, foo"})));
    Outcome first = readOutcome(fd, 1);
    ASSERT_EQ(first.responses.size(), 1u) << dump(first);
    EXPECT_FALSE(first.closed)
        << "an HTTP/1.0 client that asked for keep-alive was disconnected. Got:\n" << dump(first);
    EXPECT_EQ(connectionField(first.responses[0]), "keep-alive")
        << "HTTP/1.0 defaults to close, so persistence has to be advertised or "
           "the client will not reuse the connection. Got:\n" << dump(first);

    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.0", {"Connection: close"})));
    const Outcome second = readOutcome(fd, 1);
    EXPECT_EQ(second.responses.size(), 1u) << dump(second);
}

// ---------------------------------------------------------------------------
// L14: the response states the decision
// ---------------------------------------------------------------------------

TEST_F(ConnectionDispositionTest, ABodyResponseToCloseSaysClose) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.1", {"Connection: close"})));
    const Outcome o = readOutcome(fd, 1);

    ASSERT_EQ(o.responses.size(), 1u) << dump(o);
    EXPECT_EQ(connectionField(o.responses[0]), "close")
        << "the server closes after this response; an HTTP/1.1 response that "
           "does not say so tells the client the connection persists. Got:\n" << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, Http10WithoutKeepAliveSaysClose) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.0", {})));
    const Outcome o = readOutcome(fd, 1);

    ASSERT_EQ(o.responses.size(), 1u) << dump(o);
    EXPECT_EQ(connectionField(o.responses[0]), "close") << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, AHandlerCanCloseTheConnection) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/handler-close", "HTTP/1.1", {}) + get("/body", "HTTP/1.1", {})));
    const Outcome o = readOutcome(fd, 1);

    ASSERT_GE(o.responses.size(), 1u) << dump(o);
    EXPECT_EQ(connectionField(o.responses[0]), "close") << dump(o);
    EXPECT_EQ(o.responses.size(), 1u)
        << "the response said close and the connection kept serving. Got:\n" << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, AHandlerCannotClaimPersistenceTheServerWillNotGive) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/handler-keepalive", "HTTP/1.1", {"Connection: close"})));
    const Outcome o = readOutcome(fd, 1);

    ASSERT_EQ(o.responses.size(), 1u) << dump(o);
    EXPECT_EQ(connectionField(o.responses[0]), "close")
        << "the server is closing; a response that says keep-alive is false. Got:\n" << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------

TEST_F(ConnectionDispositionTest, Http11DefaultStillPersistsWithoutAHeader) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.1", {}) + get("/body", "HTTP/1.1", {})));
    const Outcome o = readOutcome(fd, 2);

    EXPECT_EQ(o.responses.size(), 2u) << dump(o);
    EXPECT_FALSE(o.closed) << dump(o);
    for (const auto& r : o.responses) {
        EXPECT_EQ(connectionField(r), "") << "HTTP/1.1 persistence is the default and needs no header";
    }
}

TEST_F(ConnectionDispositionTest, PlainCloseStillCloses) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.1", {"Connection: close"}) +
                            get("/body", "HTTP/1.1", {})));
    const Outcome o = readOutcome(fd, 1);
    EXPECT_EQ(o.responses.size(), 1u) << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, LookAlikeOptionsDoNotClose) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/body", "HTTP/1.1", {"Connection: closed, x-close, close-ish"}) +
                            get("/body", "HTTP/1.1", {})));
    const Outcome o = readOutcome(fd, 2);
    EXPECT_EQ(o.responses.size(), 2u)
        << "options are matched as whole tokens, not substrings. Got:\n" << dump(o);
    EXPECT_FALSE(o.closed) << dump(o);
}

TEST_F(ConnectionDispositionTest, AFileResponseToCloseStillSaysClose) {
    const int fd = connectClient();
    ASSERT_TRUE(sendAll(fd, get("/file", "HTTP/1.1", {"Connection: close"})));
    const Outcome o = readOutcome(fd, 1);
    ASSERT_EQ(o.responses.size(), 1u) << dump(o);
    EXPECT_EQ(connectionField(o.responses[0]), "close") << dump(o);
    EXPECT_TRUE(o.closed) << dump(o);
}

}  // namespace
