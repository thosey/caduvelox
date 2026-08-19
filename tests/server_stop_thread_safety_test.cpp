#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <liburing.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace caduvelox;
using namespace std::chrono_literals;

/**
 * Deterministic coverage for review item C2: Server::stop() raced the ring
 * thread on the submission queue.
 *
 * The old stop() called io_uring_get_sqe() / io_uring_prep_nop() /
 * io_uring_submit() to wake a blocked event loop. stop() runs on a foreign
 * thread (HttpServer::stop() -> ServiceRing::stop(), and tests call it from
 * main), and liburing's SQ is not thread-safe: the ring thread can be inside
 * registerJob() at the same instant, so both threads can be handed the same SQE
 * or corrupt the SQ tail.
 *
 * The wake-up is now a write(2) to an eventfd that run() polls from the ring
 * thread. The tests below pin the two properties that matter: stop() puts
 * nothing on the SQ, and it still wakes run() from every ordering.
 */
class ServerStopThreadSafetyTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
    }

    // True if no completion shows up within the timeout, i.e. nothing was
    // submitted to the ring.
    static bool ringIsQuiet(Server& server, int timeout_ms = 300) {
        struct __kernel_timespec ts{};
        ts.tv_nsec = static_cast<long long>(timeout_ms) * 1000 * 1000;
        struct io_uring_cqe* cqe = nullptr;
        return io_uring_wait_cqe_timeout(server.getRing(), &cqe, &ts) < 0;
    }

    // Run `fn` on its own thread and fail rather than hang if it never returns.
    static bool finishesWithin(std::chrono::milliseconds limit, std::function<void()> fn) {
        auto done = std::async(std::launch::async, std::move(fn));
        return done.wait_for(limit) == std::future_status::ready;
    }
};

// ---------------------------------------------------------------------------
// The core invariant: stop() never touches the submission queue
// ---------------------------------------------------------------------------

TEST_F(ServerStopThreadSafetyTest, StopSubmitsNothingToTheSubmissionQueue) {
    Server server;
    ASSERT_TRUE(server.init(64));

    // run() was never called, so nothing has armed the stop poll and nothing
    // else is in flight. Any CQE here can only have come from stop() itself.
    server.stop();

    EXPECT_TRUE(ringIsQuiet(server))
        << "stop() produced a completion, so it wrote to the SQ from this thread — "
           "the race C2 describes";
    EXPECT_EQ(server.getServerState(), ServerState::Stopping)
        << "the state transition must still happen";
}

TEST_F(ServerStopThreadSafetyTest, RepeatedStopsStillSubmitNothing) {
    Server server;
    ASSERT_TRUE(server.init(64));

    // ~Server() calls stop() after HttpServer::stop() already did, so the
    // repeat path is real. The eventfd counter just saturates; the old code
    // burned an SQE every time and could exhaust a shallow ring.
    for (int i = 0; i < 100; ++i) {
        server.stop();
    }

    EXPECT_TRUE(ringIsQuiet(server));
}

// ---------------------------------------------------------------------------
// The wake-up still works, from every ordering
// ---------------------------------------------------------------------------

TEST_F(ServerStopThreadSafetyTest, StopFromForeignThreadWakesBlockedRun) {
    Server server;
    ASSERT_TRUE(server.init(64));

    std::atomic<bool> loop_entered{false};
    server.setStartupFn([&](Server&) { loop_entered.store(true); });

    auto ran = std::async(std::launch::async, [&] { server.run(); });

    // Wait until run() is actually parked in io_uring_wait_cqe(), which is the
    // state the wake-up exists for.
    while (!loop_entered.load()) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(50ms);

    server.stop();

    ASSERT_EQ(ran.wait_for(5s), std::future_status::ready)
        << "run() never woke up — the eventfd signal did not reach the poll";
}

TEST_F(ServerStopThreadSafetyTest, StopBeforeRunIsNotLost) {
    Server server;
    ASSERT_TRUE(server.init(64));

    // Signal first, arm second. The eventfd counter persists, so the poll that
    // run() arms completes immediately instead of waiting forever for an event
    // that already happened.
    server.stop();

    EXPECT_TRUE(finishesWithin(5s, [&] { server.run(); }))
        << "run() blocked on a stop that was signalled before the poll was armed";
}

TEST_F(ServerStopThreadSafetyTest, ShutdownSweepRunsOnTheRingThread) {
    Server server;
    ASSERT_TRUE(server.init(64));

    std::atomic<std::thread::id> sweep_thread{};
    std::atomic<int> sweep_count{0};
    server.setShutdownSweepFn([&](Server&) {
        sweep_thread.store(std::this_thread::get_id());
        sweep_count.fetch_add(1);
    });

    std::atomic<bool> loop_entered{false};
    server.setStartupFn([&](Server&) { loop_entered.store(true); });

    std::thread::id ring_thread_id;
    auto ran = std::async(std::launch::async, [&] {
        ring_thread_id = std::this_thread::get_id();
        server.run();
    });

    while (!loop_entered.load()) {
        std::this_thread::sleep_for(1ms);
    }
    server.stop();
    ASSERT_EQ(ran.wait_for(5s), std::future_status::ready);

    // The sweep walks ring-local job pools, so running it on the caller's
    // thread would touch another thread's thread_local storage.
    EXPECT_EQ(sweep_count.load(), 1);
    EXPECT_EQ(sweep_thread.load(), ring_thread_id);
}

// ---------------------------------------------------------------------------
// The race itself: a foreign stop() concurrent with ring-thread submissions
// ---------------------------------------------------------------------------

namespace {

// Minimal job that submits a NOP and frees itself. Enough to keep the ring
// thread inside registerJob()/submit() while stop() runs elsewhere.
class NopJob : public IoJob {
public:
    explicit NopJob(std::atomic<int>& completions) : completions_(completions) {}

    void prepareSqe(struct io_uring_sqe* sqe) override { io_uring_prep_nop(sqe); }

    std::optional<CleanupCallback> handleCompletion(Server&, struct io_uring_cqe*) override {
        completions_.fetch_add(1, std::memory_order_relaxed);
        return +[](IoJob* job) { delete job; };
    }

private:
    std::atomic<int>& completions_;
};

}  // namespace

TEST_F(ServerStopThreadSafetyTest, ForeignStopDuringRingThreadSubmissions) {
    Server server;
    ASSERT_TRUE(server.init(256));

    std::atomic<int> completions{0};
    std::atomic<bool> submitting{false};

    // The startup fn runs on the ring thread before the event loop, so this
    // keeps the SQ busy from exactly the thread that owns it. Under the old
    // stop(), the foreign NOP submission below overlapped these calls.
    server.setStartupFn([&](Server& s) {
        submitting.store(true);
        for (int i = 0; i < 500; ++i) {
            auto* job = new NopJob(completions);
            struct io_uring_sqe* sqe = s.registerJob(job);
            if (!sqe) {
                delete job;
                s.submit();
                continue;
            }
            job->prepareSqe(sqe);
            s.submit();
        }
    });

    auto ran = std::async(std::launch::async, [&] { server.run(); });

    while (!submitting.load()) {
        std::this_thread::sleep_for(100us);
    }
    // Hammer stop() while the ring thread is mid-submission.
    for (int i = 0; i < 200; ++i) {
        server.stop();
    }

    ASSERT_EQ(ran.wait_for(10s), std::future_status::ready)
        << "run() did not exit — a corrupted SQ entry would strand in_flight_ "
           "accounting and hang drainCompletions()";

    // drainCompletions() only returns once every registered job has completed,
    // so a lost or clobbered SQE would have shown up as a hang above rather
    // than a short count. Assert the count anyway to catch a silent drop.
    EXPECT_EQ(completions.load(), 500);
}
