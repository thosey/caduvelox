#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"

using namespace caduvelox;

/**
 * Listening twice on one HttpServer (review item L23).
 *
 * listen() and listenKTLS() both guarded with `if (!isStopped())`, and a fresh
 * HttpServer is constructed Stopped while stop() returns it to Stopped. That one
 * check therefore permitted the first call and a second one after a restart
 * alike -- it cannot tell "never started" from "stopped after running".
 *
 * Nothing reset the ring state between them. startRings() only ever reserves and
 * push_back()s, so the second call appended a whole second set of rings to the
 * first: twice the configured number, half of them belonging to the previous
 * incarnation with already-stopped, already-drained Servers, and run() then
 * calling ring->start() on all of them. getNumRings() kept reporting
 * config_.num_rings, which no longer matched. listenKTLS() additionally assigned
 * ssl_ctx_ over the previous context without freeing it, leaking an OpenSSL
 * context per restart.
 *
 * That last one is real by construction -- the assignment in listenKTLS() is the
 * only write and the destructor's freeContext() is the only free -- but it is
 * invisible to the leak gate, because asan_suppressions.txt suppresses
 * libssl.so, libcrypto.so and CRYPTO_malloc, which is where an SSL_CTX is
 * allocated. Measured: with the old guard in place these tests fail on the
 * refusal and LSAN reports nothing. So no test here asserts the absence of that
 * leak; what they assert is the refusal, and the guard is called before the
 * context is created so there is no second context to lose.
 *
 * A restart is now refused and says so, rather than being made to work: the
 * previous rings' threads would have to be joined before the
 * SingleRingHttpServer objects their completion handlers dereference are
 * destroyed, which is the opposite of the declaration order the normal teardown
 * relies on, and that ordering is where review item L11 lived.
 */
namespace {

class HttpServerRestartTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        port_ = BASE_PORT + counter_++;
        second_port_ = port_ + 50;

        ServerConfig cfg;
        cfg.num_rings = 2;   // more than one, so a doubled set is unambiguous
        server_ = std::make_unique<HttpServer>(cfg);
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (thread_.joinable()) thread_.join();
        server_.reset();
    }

    // True if a plain socket can bind `port`, i.e. nothing is listening on it.
    // A SO_REUSEPORT listener still holds the port against a plain bind, so this
    // detects a ring whose listening socket was never closed.
    static bool portIsFree(uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        const bool bound =
            ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        ::close(fd);
        return bound;
    }

    static constexpr uint16_t BASE_PORT = 19400;
    static int counter_;

    uint16_t port_ = 0;
    uint16_t second_port_ = 0;
    std::unique_ptr<HttpServer> server_;
    std::thread thread_;
};

int HttpServerRestartTest::counter_ = 0;

TEST_F(HttpServerRestartTest, AFreshServerCanListen) {
    // The guard: refusing a restart must not refuse the first call.
    EXPECT_TRUE(server_->listen(port_, "127.0.0.1"));
    EXPECT_EQ(server_->getNumRings(), 2);
}

TEST_F(HttpServerRestartTest, ListeningTwiceAfterStopIsRefused) {
    ASSERT_TRUE(server_->listen(port_, "127.0.0.1"));
    server_->stop();
    ASSERT_TRUE(server_->isStopped()) << "precondition: stop() returns the server to Stopped, "
                                         "which is what the old guard could not distinguish "
                                         "from a server that had never started";

    EXPECT_FALSE(server_->listen(second_port_, "127.0.0.1"))
        << "a second listen appended another full set of rings to the first, "
           "leaving twice the configured number with half of them stopped";

    // The refusal has to leave the count honest. Pre-fix this was still 2 while
    // service_rings_ held 4, because getNumRings() reports the config.
    EXPECT_EQ(server_->getNumRings(), 2);

    // And the refused call must not have bound anything.
    EXPECT_TRUE(portIsFree(second_port_))
        << "the refused listen bound port " << second_port_ << " anyway";
}

TEST_F(HttpServerRestartTest, ListeningTwiceWithoutStoppingIsRefused) {
    // The case the original guard did catch. Kept so the rewritten guard is
    // shown to still cover it.
    ASSERT_TRUE(server_->listen(port_, "127.0.0.1"));
    EXPECT_FALSE(server_->listen(second_port_, "127.0.0.1"));
    EXPECT_TRUE(portIsFree(second_port_));
}

TEST_F(HttpServerRestartTest, ARefusedRestartLeavesTheFirstPortListening) {
    // The refusal must not disturb what is already serving: the rings from the
    // first listen are untouched, so the original port stays bound.
    ASSERT_TRUE(server_->listen(port_, "127.0.0.1"));
    ASSERT_FALSE(portIsFree(port_)) << "precondition: the first listen bound the port";

    EXPECT_FALSE(server_->listen(second_port_, "127.0.0.1"));
    EXPECT_FALSE(portIsFree(port_))
        << "the refused call tore down the listener the first call established";
}

// ---------------------------------------------------------------------------
// The TLS path, where the second call also leaked a context
// ---------------------------------------------------------------------------

class HttpServerRestartKTLSTest : public HttpServerRestartTest {};

TEST_F(HttpServerRestartKTLSTest, ListeningTwiceOverTlsIsRefusedBeforeASecondContextIsCreated) {
    ASSERT_TRUE(server_->listenKTLS(port_, CADUVELOX_TEST_CERT, CADUVELOX_TEST_KEY, "127.0.0.1"))
        << "could not start the TLS listener";
    server_->stop();
    ASSERT_TRUE(server_->isStopped());

    // Pre-fix this created a second SSL_CTX and assigned it over the first,
    // which was never freed. The guard therefore runs before the context is
    // created rather than after, so the overwrite cannot happen -- an ordering
    // invariant in the source, not something this test can observe, since a
    // refusal returns false either way and the leak gate cannot see an
    // SSL_CTX (see the note at the top of this file).
    EXPECT_FALSE(server_->listenKTLS(second_port_, CADUVELOX_TEST_CERT, CADUVELOX_TEST_KEY,
                                     "127.0.0.1"));
    EXPECT_TRUE(portIsFree(second_port_));
}

TEST_F(HttpServerRestartKTLSTest, AFreshServerCanListenOverTls) {
    EXPECT_TRUE(server_->listenKTLS(port_, CADUVELOX_TEST_CERT, CADUVELOX_TEST_KEY, "127.0.0.1"));
}

}  // namespace
