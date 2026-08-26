// Review item M13 — SIGPIPE must not be able to kill the server.
//
// Writing to a socket whose peer has closed raises SIGPIPE, and IORING_OP_WRITE is
// no exception. WriteJob submits every HTTP response through io_uring_prep_write()
// on a client socket, so under the default disposition a client that disconnects
// mid-response terminates the whole process — no log line, no drain.
//
// This was not hypothetical: the pre-C6 pipelining code died with signal 13 in
// HttpResponseOrderingTest, reproducibly, while a control test in the same binary
// exited 0. Server::init() now ignores SIGPIPE once per process, which turns the
// condition into an ordinary -EPIPE delivered through WriteJob's error callback.
//
// Measured for reference (socketpair, peer closed, kernel 7.1):
//   io_uring_prep_write                  -> dies with signal 13
//   io_uring_prep_write + SIGPIPE ignored -> res = -EPIPE
//   io_uring_prep_send with MSG_NOSIGNAL  -> res = -EPIPE
//   io_uring_prep_splice (pipe -> socket) -> res = -EPIPE
// WriteJob's op is the only one in this codebase that needs the guard.

#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/WriteJob.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

// A pair of connected sockets whose peer end is closed on demand.
struct DeadPeerSocket {
    int local = -1;

    DeadPeerSocket() {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            local = sv[0];
            ::close(sv[1]);  // peer is gone: any write must fail with EPIPE
        }
    }
    ~DeadPeerSocket() {
        if (local >= 0) ::close(local);
    }
};

TEST(SigPipeGuardTest, ServerInitIgnoresSigPipe) {
    caduvelox::Server server;
    ASSERT_TRUE(server.init(64, 64, 4096));

    struct sigaction current{};
    ASSERT_EQ(sigaction(SIGPIPE, nullptr, &current), 0);
    EXPECT_EQ(current.sa_handler, SIG_IGN)
        << "Server::init() must ignore SIGPIPE; otherwise a peer that disconnects "
           "mid-response kills the process.";
}

// The behavioural half: drive a real WriteJob at a socket with no peer and show the
// process survives and the failure is reported as EPIPE. Without the guard in
// Server::init() this test does not fail -- it takes the whole test binary down.
//
// The job is created and the event loop is run on this same thread: WriteJob is
// pool-allocated from a thread_local pool, so it must be freed on the thread that
// allocated it (in production that is always the ring thread, from a completion
// handler). The error callback stops the loop, so run() returns on its own.
TEST(SigPipeGuardTest, WriteToClosedPeerReportsEpipeInsteadOfKillingProcess) {
    DeadPeerSocket sock;
    ASSERT_GE(sock.local, 0);

    caduvelox::Server server;
    ASSERT_TRUE(server.init(64, 64, 4096));

    std::atomic<int> reported_error{0};
    std::atomic<bool> completed_ok{false};
    std::atomic<bool> done{false};

    auto* job = caduvelox::WriteJob::createFromPoolFromString(
        sock.local, std::string(64 * 1024, 'x'),
        [&](int, size_t) { completed_ok = true; done = true; server.stop(); },
        [&](int, int err) { reported_error = err; done = true; server.stop(); });
    ASSERT_NE(job, nullptr) << "WriteJob pool exhausted";

    job->start(server);

    // Watchdog so a regression hangs the test rather than the suite. stop() is
    // safe to call from another thread (it is an eventfd write -- review item C2).
    std::thread watchdog([&] {
        for (int i = 0; i < 500 && !done.load(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
        if (!done.load()) server.stop();
    });

    server.run();
    watchdog.join();

    ASSERT_TRUE(done.load()) << "WriteJob never completed";
    EXPECT_FALSE(completed_ok.load()) << "write to a peerless socket should not succeed";
    EXPECT_EQ(reported_error.load(), EPIPE)
        << "expected the failure to surface as EPIPE through on_error_";
}

}  // namespace
