#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
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
 * A delivered error response is a response, not a broken connection
 * (review item M11).
 *
 * HTTPFileJob::sendError() reported a *successfully written* error response
 * through on_error_, passing 0 as the error code to mean "handled". Nothing
 * downstream read that code: HttpConnectionJob's error callback closes the
 * connection whatever it says. So every 404 tore down a connection whose
 * response had gone out intact and whose client was entitled to keep using it.
 *
 * A missing file is an ordinary answer to an ordinary request. The client that
 * pays for this is the one doing what HTTP/1.1 asks -- reusing a connection
 * across a set of resources, some of which happen not to exist -- and under
 * TLS it pays a full handshake per miss.
 *
 * These drive a real server over a real socket, because the bug is only visible
 * in what happens to the connection after the response.
 */
namespace {

class FileErrorKeepAliveTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        test_port_ = BASE_PORT + test_counter_++;

        dir_ = std::filesystem::temp_directory_path() /
               ("cadu_m11_" + std::to_string(::getpid()) + "_" + std::to_string(test_port_));
        std::filesystem::create_directories(dir_);
        {
            std::ofstream ofs(dir_ / "present.txt", std::ios::binary | std::ios::trunc);
            ofs << kBody;
        }

        ASSERT_TRUE(job_server_.init(256));
        http_server_ = std::make_unique<SingleRingHttpServer>(job_server_);

        // Maps the last path segment onto a file, which is exactly the shape of
        // route that can be asked for something that is not there.
        http_server_->addRoute("GET", "^/files/.*$",
            [this](const HttpRequest& req, HttpResponse& res) {
                const size_t slash = req.path.rfind('/');
                const std::string name =
                    (slash == std::string::npos) ? req.path : req.path.substr(slash + 1);
                res.sendFile((dir_ / name).string());
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

        timeval tv{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

    static std::string request(const std::string& path, bool close_after = false) {
        return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" +
               (close_after ? "Connection: close\r\n" : "") + "\r\n";
    }

    // Read until `count` complete responses have arrived, or the peer hangs up.
    // Returns what was read; `hung_up` says whether the server closed.
    static std::string readResponses(int fd, int count, bool& hung_up) {
        std::string buf;
        hung_up = false;
        while (countResponses(buf) < count) {
            char tmp[16384];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n == 0) { hung_up = true; break; }
            if (n < 0) {
                if (errno == EINTR) continue;
                hung_up = true;  // including a timeout: nothing more is coming
                break;
            }
            buf.append(tmp, static_cast<size_t>(n));
        }
        return buf;
    }

    // Complete responses only -- headers plus the Content-Length bytes that
    // follow. Counting status lines instead would stop the read as soon as the
    // headers landed, which for a spliced file is before any of the body.
    static int countResponses(const std::string& buf) {
        int n = 0;
        size_t pos = 0;
        for (;;) {
            const size_t header_end = buf.find("\r\n\r\n", pos);
            if (header_end == std::string::npos) break;

            std::string headers = buf.substr(pos, header_end + 4 - pos);
            for (auto& c : headers) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

            size_t body = 0;
            const size_t key = headers.find("content-length:");
            if (key != std::string::npos) {
                body = static_cast<size_t>(std::strtoul(headers.c_str() + key + 15, nullptr, 10));
            }

            const size_t total = (header_end + 4 - pos) + body;
            if (buf.size() - pos < total) break;  // body still arriving
            pos += total;
            ++n;
        }
        return n;
    }

    // Is the connection still open? A zero-length read means the peer closed.
    static bool stillOpen(int fd) {
        timeval tv{ .tv_sec = 0, .tv_usec = 300 * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char tmp[1];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        timeval restore{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &restore, sizeof(restore));
        if (n == 0) return false;                       // orderly shutdown
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;  // quiet, open
        return n > 0;                                   // unexpected data, but open
    }

    static constexpr const char* kBody = "the quick brown fox";
    static constexpr uint16_t BASE_PORT = 9900;
    static int test_counter_;

    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_server_;
    std::thread server_thread_;
    std::filesystem::path dir_;
    uint16_t test_port_ = 0;
    int client_fd_ = -1;
};

int FileErrorKeepAliveTest::test_counter_ = 0;

// ---------------------------------------------------------------------------
// The bug
// ---------------------------------------------------------------------------

TEST_F(FileErrorKeepAliveTest, AMissingFileLeavesTheConnectionUsable) {
    const int fd = connectClient();

    ASSERT_TRUE(sendAll(fd, request("/files/absent.txt")));
    bool hung_up = false;
    const std::string first = readResponses(fd, 1, hung_up);
    ASSERT_NE(first.find("HTTP/1.1 404"), std::string::npos) << "got:\n" << first;

    EXPECT_TRUE(stillOpen(fd))
        << "the 404 was written to the socket in full, and the client asked for "
           "keep-alive. Closing here reports a delivered response as a "
           "connection-level failure.";

    ASSERT_TRUE(sendAll(fd, request("/files/present.txt")))
        << "the connection was already gone";
    const std::string second = readResponses(fd, 1, hung_up);
    EXPECT_NE(second.find("HTTP/1.1 200"), std::string::npos) << "got:\n" << second;
    EXPECT_NE(second.find(kBody), std::string::npos) << "got:\n" << second;
}

TEST_F(FileErrorKeepAliveTest, APipelinedMissAndHitBothGetAnswered) {
    const int fd = connectClient();

    // Both in one write, so the server has them together and cannot claim it
    // simply never saw the second.
    ASSERT_TRUE(sendAll(fd, request("/files/absent.txt") + request("/files/present.txt")));

    bool hung_up = false;
    const std::string both = readResponses(fd, 2, hung_up);

    EXPECT_EQ(countResponses(both), 2)
        << "the first request 404'd and the connection went away, taking the "
           "already-pipelined second request with it. Got:\n" << both;
    EXPECT_NE(both.find("HTTP/1.1 404"), std::string::npos) << both;
    EXPECT_NE(both.find("HTTP/1.1 200"), std::string::npos) << both;
    EXPECT_NE(both.find(kBody), std::string::npos) << both;
}

TEST_F(FileErrorKeepAliveTest, SeveralMissesInARowStayOnOneConnection) {
    const int fd = connectClient();

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(sendAll(fd, request("/files/absent" + std::to_string(i) + ".txt")))
            << "connection died after " << i << " misses";
        bool hung_up = false;
        const std::string res = readResponses(fd, 1, hung_up);
        ASSERT_NE(res.find("HTTP/1.1 404"), std::string::npos)
            << "miss " << i << " got:\n" << res;
    }

    ASSERT_TRUE(sendAll(fd, request("/files/present.txt")));
    bool hung_up = false;
    const std::string res = readResponses(fd, 1, hung_up);
    EXPECT_NE(res.find("HTTP/1.1 200"), std::string::npos) << "got:\n" << res;
}

// ---------------------------------------------------------------------------
// Guards: keeping the connection must not mean keeping it always
// ---------------------------------------------------------------------------

TEST_F(FileErrorKeepAliveTest, ConnectionCloseIsStillHonouredOnAnError) {
    const int fd = connectClient();

    ASSERT_TRUE(sendAll(fd, request("/files/absent.txt", /*close_after=*/true)));
    bool hung_up = false;
    const std::string res = readResponses(fd, 1, hung_up);
    ASSERT_NE(res.find("HTTP/1.1 404"), std::string::npos) << "got:\n" << res;

    // The error reply is built fresh rather than from the response the
    // connection prepared, so it used to drop this header and leave the client
    // to assume persistence right up until the close.
    EXPECT_NE(res.find("connection: close"), std::string::npos)
        << "an error response has to advertise the close it is about to do. "
           "Got:\n" << res;

    EXPECT_FALSE(stillOpen(fd))
        << "the client asked for the connection to be closed after this response";
}

TEST_F(FileErrorKeepAliveTest, ConnectionCloseIsStillHonouredOnASuccess) {
    const int fd = connectClient();

    ASSERT_TRUE(sendAll(fd, request("/files/present.txt", /*close_after=*/true)));
    bool hung_up = false;
    const std::string res = readResponses(fd, 1, hung_up);
    ASSERT_NE(res.find("HTTP/1.1 200"), std::string::npos) << "got:\n" << res;

    EXPECT_FALSE(stillOpen(fd));
}

TEST_F(FileErrorKeepAliveTest, ASuccessfulFileStillKeepsTheConnection) {
    const int fd = connectClient();

    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(sendAll(fd, request("/files/present.txt")))
            << "connection died after " << i << " successful transfers";
        bool hung_up = false;
        const std::string res = readResponses(fd, 1, hung_up);
        ASSERT_NE(res.find("HTTP/1.1 200"), std::string::npos) << "got:\n" << res;
        ASSERT_NE(res.find(kBody), std::string::npos) << "got:\n" << res;
    }
}

}  // namespace
