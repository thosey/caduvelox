#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
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
 * Registering a route after the server is listening (review item L20).
 *
 * It used to be accepted and then do nothing. HttpServer::startRings() copies the
 * router into each ring as it starts, so a later route reached only the parent's
 * copy, which nothing reads again -- the route simply never matched, with no error
 * anywhere. examples/rest_api_server was doing exactly that, which is how it was
 * noticed: the header already said "Must be called before listenKTLS()", and a
 * comment was not enough.
 *
 * On SingleRingHttpServer a late route would instead take effect, because
 * connection jobs hold a reference to that object's router -- which is worse,
 * since it mutates a regex vector while the ring thread may be matching against
 * it. Both now refuse, so the two classes behave alike.
 */
namespace {

class CapturingLogger : public Logger {
public:
    void logMessage(std::string_view) override {}
    void logError(std::string_view msg) override {
        std::lock_guard<std::mutex> lock(mutex_);
        errors_.emplace_back(msg);
    }
    bool sawError(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& e : errors_) {
            if (e.find(needle) != std::string::npos) return true;
        }
        return false;
    }
private:
    std::mutex mutex_;
    std::vector<std::string> errors_;
};

// Restores the global logger, which is process-wide.
class LateRouteTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::setGlobalLogger(&capture_); }
    void TearDown() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
    }

    static std::string get(uint16_t port, const std::string& target) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
        timeval tv{ .tv_sec = 3, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const std::string req =
            "GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, static_cast<size_t>(n));
        ::close(fd);
        return out;
    }

    CapturingLogger capture_;
};

TEST_F(LateRouteTest, MultiRingServerRefusesARouteAddedAfterListening) {
    const uint16_t port = 19100;
    ServerConfig cfg;
    cfg.num_rings = 1;
    HttpServer server(cfg);
    server.addRoute("GET", "^/early$", [](const HttpRequest&, HttpResponse& res) {
        res.setBody("early");
    });
    ASSERT_TRUE(server.listen(port, "127.0.0.1"));

    // Too late: this used to be accepted and silently never match.
    server.addRoute("GET", "^/late$", [](const HttpRequest&, HttpResponse& res) {
        res.setBody("late");
    });

    std::thread ring([&] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    EXPECT_NE(get(port, "/early").find("HTTP/1.1 200"), std::string::npos)
        << "the route registered before listening should still work";
    EXPECT_NE(get(port, "/late").find("HTTP/1.1 404"), std::string::npos)
        << "the late route must not match -- it never reached the rings";
    EXPECT_TRUE(capture_.sawError("addRoute() called after"))
        << "the refusal has to be visible; silence is the bug being fixed";

    server.stop();
    ring.join();
}

TEST_F(LateRouteTest, SingleRingServerRefusesARouteAddedAfterListening) {
    const uint16_t port = 19101;
    Server job_server;
    ASSERT_TRUE(job_server.init(256));
    SingleRingHttpServer http(job_server);
    http.addRoute("GET", "^/early$", [](const HttpRequest&, HttpResponse& res) {
        res.setBody("early");
    });
    ASSERT_TRUE(http.listen(port, "127.0.0.1"));

    // Would have taken effect here, by mutating a router the ring thread reads.
    http.addRoute("GET", "^/late$", [](const HttpRequest&, HttpResponse& res) {
        res.setBody("late");
    });

    std::thread ring([&] { job_server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_NE(get(port, "/early").find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(get(port, "/late").find("HTTP/1.1 404"), std::string::npos)
        << "accepting this would mean editing the router while it is being matched";
    EXPECT_TRUE(capture_.sawError("addRoute() called after listen()"));

    http.stop();
    job_server.stop();
    ring.join();
}

// Guard: capture routes are refused the same way, and the ordinary order works.
TEST_F(LateRouteTest, CaptureRoutesFollowTheSameRule) {
    const uint16_t port = 19102;
    ServerConfig cfg;
    cfg.num_rings = 1;
    HttpServer server(cfg);
    server.addRouteWithCaptures("GET", R"(^/echo/(.+)$)",
        [](const HttpRequest&, HttpResponse& res, const std::smatch& m) {
            res.setBody(m[1].str());
        });
    ASSERT_TRUE(server.listen(port, "127.0.0.1"));
    server.addRouteWithCaptures("GET", R"(^/late/(.+)$)",
        [](const HttpRequest&, HttpResponse& res, const std::smatch& m) {
            res.setBody(m[1].str());
        });

    std::thread ring([&] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    EXPECT_NE(get(port, "/echo/hi").find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(get(port, "/late/hi").find("HTTP/1.1 404"), std::string::npos);
    EXPECT_TRUE(capture_.sawError("addRouteWithCaptures() called after"));

    server.stop();
    ring.join();
}

}  // namespace
