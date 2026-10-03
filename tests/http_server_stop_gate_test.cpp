#include <gtest/gtest.h>

#include <memory>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "caduvelox/Server.hpp"
#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/ServiceRing.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"

/**
 * HttpServer::stop() must do the work even when it is not the one announcing it
 * (review items L11, L21).
 *
 * stop()'s CAS used to gate the shutdown, not just the log line. state_ is
 * shared with every ring's Server -- startRings() binds each one to this same
 * atomic -- so any single ring calling Server::stop() already flips it to
 * Stopping. A later HttpServer::stop(), including the one in ~HttpServer(),
 * then found the CAS failing and returned without stopping the *other* rings or
 * closing their listening sockets.
 *
 * This is the one defect in the review that cannot be reached from outside the
 * class. It needs a ring to flip the shared state on its own, and HttpServer
 * exposes no handle to an individual ring; calling stop() twice does not do it,
 * because the first call performs the full shutdown anyway. Hence the friend
 * declaration in HttpServer -- see the note there for why a friend rather than
 * an accessor.
 *
 * The fixture is the friend, so every privileged touch lives in a helper here:
 * TEST_F generates a subclass, and a subclass of a friend is not a friend.
 * The fixture also has to sit in namespace caduvelox and outside any anonymous
 * namespace, or it would be a different class from the one the header names.
 *
 * No run() anywhere below. The rings are listening but their threads are never
 * started, which keeps the thing under test -- did stop() close the listeners --
 * free of any race with a draining event loop.
 *
 * Only ONE of these four discriminates against the pre-fix gate, and the reason
 * is worth recording: ~SingleRingHttpServer() closes its own listening socket,
 * so the destruction path cleaned up regardless. The defect's observable cost
 * was confined to an explicit stop() -- which returned having stopped nothing,
 * leaving every other ring listening for as long as the object lived.
 */
namespace caduvelox {

class HttpServerStopGateTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        port_ = BASE_PORT + counter_++;

        ServerConfig cfg;
        cfg.num_rings = 2;   // more than one, so "the other rings" exist
        // Deliberately tiny. At the default 4096-deep ring with a 512 x 16 KiB
        // buffer ring, several of these fixtures running under ctest -j 2
        // exhausted the locked-memory limit and ring init failed with ENOMEM --
        // measured: "ServiceRing[2]: Failed to initialize io_uring: Cannot
        // allocate memory". None of these tests moves a byte of traffic.
        cfg.queue_depth = 64;
        cfg.buffer_ring_count = 16;
        cfg.buffer_size_bytes = 4096;
        server_ = std::make_unique<HttpServer>(cfg);
    }

    void TearDown() override {
        server_.reset();
    }

    // Stop ONE ring's Server, and nothing else. This is what a ring does to
    // itself; it flips the shared state_ to Stopping without touching the
    // ServiceRing's own running_ flag or any other ring.
    void stopOneRingServer() {
        ASSERT_FALSE(server_->service_rings_.empty());
        server_->service_rings_.front()->getServer().stop();
    }

    size_t ringCount() const { return server_->service_rings_.size(); }

    // True if a plain socket can bind the port, i.e. no listener holds it. A
    // SO_REUSEPORT listener still refuses a plain bind, so this detects a ring
    // whose listening socket was never closed.
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

    static constexpr uint16_t BASE_PORT = 19600;
    static int counter_;

    uint16_t port_ = 0;
    std::unique_ptr<HttpServer> server_;
};

int HttpServerStopGateTest::counter_ = 0;

TEST_F(HttpServerStopGateTest, StopClosesTheListenersAfterARingFlippedTheSharedState) {
    ASSERT_TRUE(server_->listen(port_, "127.0.0.1"));
    ASSERT_EQ(ringCount(), 2u);
    ASSERT_TRUE(server_->isRunning());
    ASSERT_FALSE(portIsFree(port_)) << "precondition: the rings are listening";

    // One ring stops itself. The shared state_ is now Stopping, so the CAS in
    // HttpServer::stop() below will fail.
    stopOneRingServer();
    ASSERT_TRUE(server_->isStopping())
        << "precondition: a single ring's Server::stop() flips the state shared "
           "with HttpServer, which is the whole reason the CAS cannot gate the work";

    server_->stop();

    EXPECT_TRUE(portIsFree(port_))
        << "stop() returned without closing the listening sockets, because its "
           "CAS had already been lost to a ring that stopped itself. The other "
           "rings keep running and the port stays bound.";
}

// Guard, not a discriminator -- measured: this passes against the pre-fix CAS
// gate too. ~SingleRingHttpServer() calls its own stop(), which closes that
// ring's listening socket, so member destruction cleans up whether or not
// HttpServer::stop() ran its loops. The CAS defect's observable cost is
// therefore confined to an explicit stop(), where the listeners stayed open for
// as long as the object lived -- a server told to stop that kept accepting.
// Kept because the destruction path is worth pinning on its own.
TEST_F(HttpServerStopGateTest, TheDestructorAlsoClosesThemAfterARingFlippedTheState) {
    ASSERT_TRUE(server_->listen(port_, "127.0.0.1"));
    ASSERT_FALSE(portIsFree(port_));

    stopOneRingServer();
    ASSERT_TRUE(server_->isStopping());

    server_.reset();   // ~HttpServer() -> stop()

    EXPECT_TRUE(portIsFree(port_))
        << "the listeners outlived the server object that owned them";
}

// Guard: the ordinary path, where stop() is the one that wins the CAS, must
// still close everything. A fix that simply dropped the CAS would pass the
// tests above and could still break this.
TEST_F(HttpServerStopGateTest, AnOrdinaryStopStillClosesTheListeners) {
    ASSERT_TRUE(server_->listen(port_, "127.0.0.1"));
    ASSERT_TRUE(server_->isRunning());
    ASSERT_FALSE(portIsFree(port_));

    server_->stop();

    EXPECT_TRUE(portIsFree(port_));
}

// Guard: a server that never listened must not be disturbed by stop(), which is
// what the "never started; nothing to wind down" early return is for.
TEST_F(HttpServerStopGateTest, StopOnAServerThatNeverListenedIsHarmless) {
    ASSERT_EQ(ringCount(), 0u);
    server_->stop();
    EXPECT_TRUE(server_->isStopped());
    EXPECT_EQ(ringCount(), 0u);
}

}  // namespace caduvelox
