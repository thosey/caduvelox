#include <gtest/gtest.h>
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <string>
#include <thread>

using namespace caduvelox;

/**
 * Routing through HttpServer, the public multi-ring class (review item L17).
 *
 * HttpServer forwarded only addRoute. addRouteWithCaptures existed on HttpRouter
 * and on SingleRingHttpServer but not here, so a multi-ring application could not
 * take a regex capture -- and since the router started percent-decoding paths
 * (M12), a capture is the only thing that arrives decoded and without the query
 * string. examples/static_https_server was re-deriving it from req.path by hand
 * for want of this.
 *
 * These drive HttpServer over TLS because listenKTLS is the only listen it
 * offers. That also makes this the first coverage of routing through the
 * multi-ring class at all.
 */
namespace {

// Minimal TLS client, same shape as the one in the kTLS tests.
class TlsClient {
public:
    bool connect(uint16_t port) {
        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) return false;
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, sock_);
        return SSL_connect(ssl_) == 1;
    }

    std::string request(const std::string& target) {
        const std::string req =
            "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        if (SSL_write(ssl_, req.data(), static_cast<int>(req.size())) <= 0) return "(write failed)";
        std::string out;
        char buf[8192];
        for (;;) {
            const int n = SSL_read(ssl_, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }

    ~TlsClient() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
        if (ctx_) SSL_CTX_free(ctx_);
        if (sock_ >= 0) ::close(sock_);
    }

private:
    int sock_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

std::string bodyOf(const std::string& response) {
    const size_t end = response.find("\r\n\r\n");
    return end == std::string::npos ? std::string() : response.substr(end + 4);
}

class HttpServerRouteApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        port_ = BASE_PORT + counter_++;

        ServerConfig cfg;
        cfg.num_rings = 1;
        server_ = std::make_unique<HttpServer>(cfg);
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (thread_.joinable()) thread_.join();
    }

    void start() {
        ASSERT_TRUE(server_->listenKTLS(port_, "test_cert.pem", "test_key.pem", "127.0.0.1"))
            << "could not start the TLS listener";
        thread_ = std::thread([this] { server_->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    static constexpr uint16_t BASE_PORT = 18700;
    static int counter_;

    uint16_t port_ = 0;
    std::unique_ptr<HttpServer> server_;
    std::thread thread_;
};

int HttpServerRouteApiTest::counter_ = 0;

TEST_F(HttpServerRouteApiTest, ACaptureRouteArrivesDecodedAndWithoutTheQuery) {
    server_->addRouteWithCaptures("GET", R"(^/files/(.+)$)",
        [](const HttpRequest&, HttpResponse& res, const std::smatch& m) {
            res.setBody("[" + m[1].str() + "]");
        });
    start();

    TlsClient client;
    ASSERT_TRUE(client.connect(port_)) << "TLS handshake failed";
    const std::string response = client.request("/files/a%20b.txt?v=3");

    EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos) << response;
    EXPECT_EQ(bodyOf(response), "[a b.txt]")
        << "the capture should arrive percent-decoded and without the query. Got:\n"
        << response;
}

// Guard: the plain route API still works through the same class.
TEST_F(HttpServerRouteApiTest, APlainRouteStillWorks) {
    server_->addRoute("GET", "^/ping$", [](const HttpRequest&, HttpResponse& res) {
        res.setBody("pong");
    });
    start();

    TlsClient client;
    ASSERT_TRUE(client.connect(port_)) << "TLS handshake failed";
    const std::string response = client.request("/ping");

    EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos) << response;
    EXPECT_EQ(bodyOf(response), "pong") << response;
}

}  // namespace
