#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <string>
#include <thread>

using namespace caduvelox;

/**
 * AcceptJob really comes from its pool, on the right thread (review items M3, M4).
 *
 * AcceptJob::create() used plain `new`, while accept_pool_size,
 * PoolCapacityConfig<AcceptJob> and the [POOL_EXHAUSTED] branch in
 * startAccepting() all configured and reported on a pool nothing used. Since
 * `new` never returns null, that branch could not run.
 *
 * The cost was a real leak. The job is freed only when its multishot accept
 * delivers a terminal completion, so a ring torn down before that completion
 * drains stranded it -- 2024 bytes over 23 allocations in the multi-ring
 * shutdown test, every one from AcceptJob::create() by way of
 * HttpServer::listenKTLS(). It sat behind the only entry in
 * asan_suppressions.txt that was ours.
 *
 * Pool allocation fixes it because ThreadLocalPool destroys objects still live
 * at pool teardown -- but only if the job belongs to the pool of the thread
 * that runs the ring. listen() runs on the caller's thread, before any ring
 * thread exists, so the accept is now armed from Server::setStartupFn(), which
 * run() calls on the ring thread. That half is proven by the sanitizer gate with
 * the suppression removed, not by these tests.
 */
namespace {

class AcceptJobPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(32));
    }

    Server server_;
};

// ---------------------------------------------------------------------------
// The pool is real
// ---------------------------------------------------------------------------

TEST_F(AcceptJobPoolTest, CreateTakesASlotAndFreeReturnsIt) {
    const size_t before = PoolManager::allocated<AcceptJob>();

    AcceptJob* job = AcceptJob::create(-1, nullptr, nullptr);
    ASSERT_NE(job, nullptr);
    EXPECT_EQ(PoolManager::allocated<AcceptJob>(), before + 1)
        << "create() did not come from the pool, so accept_pool_size and the "
           "[POOL_EXHAUSTED] branch describe something that does not exist";

    AcceptJob::freePoolAllocated(job);
    EXPECT_EQ(PoolManager::allocated<AcceptJob>(), before);
}

TEST_F(AcceptJobPoolTest, TheTerminalCompletionReturnsTheSlot) {
    const size_t before = PoolManager::allocated<AcceptJob>();

    AcceptJob* job = AcceptJob::create(-1, nullptr, nullptr);
    ASSERT_NE(job, nullptr);

    struct io_uring_cqe cqe{};
    cqe.res = -EBADF;   // unrecoverable: hands back a cleanup callback
    cqe.flags = 0;
    auto cleanup = job->handleCompletion(server_, &cqe);
    ASSERT_TRUE(cleanup.has_value());
    (*cleanup)(job);

    EXPECT_EQ(PoolManager::allocated<AcceptJob>(), before)
        << "the cleanup callback did not return the slot to the pool";
}

/**
 * Capacity is honoured, which is the part that could not be true before: `new`
 * cannot report exhaustion.
 *
 * Runs on a fresh thread on purpose. A pool is thread-local and reads its
 * capacity once, when the thread first touches it, so setCapacity() only takes
 * effect for a thread that has not used the pool yet.
 */
TEST(AcceptJobPoolCapacity, ExhaustionIsReportedRatherThanIgnored) {
    static ConsoleLogger console_logger;
    Logger::setGlobalLogger(&console_logger);

    const size_t original = PoolManager::getCapacity<AcceptJob>();
    PoolManager::setCapacity<AcceptJob>(1);

    bool first_ok = false;
    bool second_refused = false;
    std::thread worker([&] {
        AcceptJob* a = AcceptJob::create(-1, nullptr, nullptr);
        first_ok = (a != nullptr);
        AcceptJob* b = AcceptJob::create(-1, nullptr, nullptr);
        second_refused = (b == nullptr);
        if (a) AcceptJob::freePoolAllocated(a);
        if (b) AcceptJob::freePoolAllocated(b);
    });
    worker.join();

    PoolManager::setCapacity<AcceptJob>(original);

    EXPECT_TRUE(first_ok) << "a pool of one should hand out one job";
    EXPECT_TRUE(second_refused)
        << "with the pool full, create() must return nullptr. Plain `new` never "
           "does, which is why the [POOL_EXHAUSTED] branch was dead code";
}

// ---------------------------------------------------------------------------
// Guard: moving the arming to the ring thread must not stop it accepting
// ---------------------------------------------------------------------------

TEST(AcceptJobArming, TheServerStillAcceptsAfterArmingMovesToTheRingThread) {
    static ConsoleLogger console_logger;
    Logger::setGlobalLogger(&console_logger);

    Server job_server;
    ASSERT_TRUE(job_server.init(256));
    SingleRingHttpServer http(job_server);
    http.addRoute("GET", "^/ping$", [](const HttpRequest&, HttpResponse& res) {
        res.setBody("pong");
    });

    const size_t before = PoolManager::allocated<AcceptJob>();
    const uint16_t port = 10200;
    ASSERT_TRUE(http.listen(port, "127.0.0.1"));

    // listen() no longer arms the accept: run() does, on the ring thread. The
    // socket is listening either way, so a client can connect before then.
    EXPECT_EQ(PoolManager::allocated<AcceptJob>(), before)
        << "the accept was allocated on this thread, not the ring thread";

    std::thread ring([&] { job_server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    timeval tv{ .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(fd, req.data(), req.size(), MSG_NOSIGNAL), static_cast<ssize_t>(req.size()));

    std::string got;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) { got.append(buf, static_cast<size_t>(n)); continue; }
        break;
    }
    ::close(fd);

    http.stop();
    job_server.stop();
    ring.join();

    EXPECT_NE(got.find("HTTP/1.1 200"), std::string::npos) << "got:\n" << got;
    EXPECT_NE(got.find("pong"), std::string::npos) << "got:\n" << got;
}

}  // namespace
