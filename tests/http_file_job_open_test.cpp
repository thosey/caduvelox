#include <gtest/gtest.h>
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <liburing.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <string>
#include <thread>

using namespace caduvelox;

/**
 * Coverage for review item H4: what HTTPFileJob agrees to open and serve.
 *
 * open(path, O_RDONLY) is not a "give me this file" call. It is a "do whatever
 * this inode says" call, and for anything that is not a regular file that means
 * something other than reading bytes:
 *
 *   - a FIFO with no writer blocks *inside open()*, on the ring thread, taking
 *     every connection that core owns down with it until someone opens the
 *     write end;
 *   - a directory opens fine, reports a plausible st_size, and only fails later
 *     inside splice — after a 200 and a Content-Length have already gone out;
 *   - a device node opens whatever driver is behind it.
 *
 * A route that maps any part of a request onto a path can reach all three.
 *
 * The tests run against a real ring but without Server::run(): they pump
 * completions themselves. start() is run under a deadline because the failure
 * it guards against is a *hang*, and a test for a hang must fail with a
 * diagnostic rather than wedge the whole suite.
 */
class HttpFileJobOpenTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(128));
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_), 0);
        dir_ = std::filesystem::temp_directory_path() /
               ("cadu_h4_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        if (sv_[0] >= 0) close(sv_[0]);
        if (sv_[1] >= 0) close(sv_[1]);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::string makeRegularFile(const std::string& name, const std::string& contents) {
        const auto p = dir_ / name;
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs << contents;
        return p.string();
    }

    std::string makeFifo(const std::string& name) {
        const auto p = dir_ / name;
        EXPECT_EQ(mkfifo(p.c_str(), 0600), 0) << "mkfifo: " << std::strerror(errno);
        return p.string();
    }

    std::string makeDirectory(const std::string& name) {
        const auto p = dir_ / name;
        std::filesystem::create_directories(p);
        // Give it a child so the directory is not empty, which is the shape a
        // path-mapping route actually produces.
        std::ofstream(p / "child.txt") << "child";
        return p.string();
    }

    HTTPFileJob* makeJob(const std::string& path) {
        return HTTPFileJob::createFromPool(
            sv_[0], path, HttpResponse{},
            [this](int, size_t bytes) {
                completed_ = true;
                bytes_sent_ = bytes;
                done_ = true;
            },
            [this](int, int error) {
                errored_ = true;
                last_error_ = error;
                done_ = true;
            });
    }

    /**
     * Run job->start(), which opens the file synchronously on *this* thread.
     *
     * Everything stays on one thread on purpose: the pools are thread-local, so
     * a job allocated here and started elsewhere would be destroyed the moment
     * that other thread exited -- with its io_uring operation still in flight.
     * The watchdog therefore only touches the filesystem.
     *
     * A FIFO with no writer blocks inside open(). Post-fix start() returns at
     * once and the watchdog never fires; pre-fix it parks until the watchdog
     * opens the other end, which is what keeps a regression from wedging the
     * whole suite instead of failing it.
     *
     * @return false if the watchdog had to release a blocked open.
     */
    bool startWithoutBlocking(HTTPFileJob* job,
                              const std::string& fifo_to_release = "",
                              std::chrono::milliseconds limit = std::chrono::milliseconds(2000)) {
        std::atomic<bool> returned{false};
        std::atomic<bool> released{false};

        std::thread watchdog;
        if (!fifo_to_release.empty()) {
            watchdog = std::thread([&] {
                const auto deadline = std::chrono::steady_clock::now() + limit;
                while (!returned.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (returned.load()) return;
                released.store(true);
                // O_RDWR on a FIFO never blocks on Linux and counts as both ends,
                // so it releases whoever is parked in the O_RDONLY open.
                int fd = open(fifo_to_release.c_str(), O_RDWR | O_NONBLOCK);
                if (fd >= 0) close(fd);
            });
        }

        job->start(server_);
        returned.store(true);
        if (watchdog.joinable()) watchdog.join();
        return !released.load();
    }

    // Dispatch completions until the job reports done. Mirrors what
    // Server::handleCompletion() does; every CQE on this ring belongs to the
    // job under test or to a child it created.
    bool pumpUntilDone(int timeout_seconds = 5) {
        while (!done_) {
            struct __kernel_timespec ts{};
            ts.tv_sec = timeout_seconds;
            struct io_uring_cqe* cqe = nullptr;
            if (io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts) < 0) {
                return false;
            }
            auto* child = reinterpret_cast<IoJob*>(io_uring_cqe_get_data64(cqe));
            auto cleanup = child->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) {
                (*cleanup)(child);
            }
        }
        return true;
    }

    // Everything the server has written to the client so far.
    std::string clientBytes(int timeout_ms = 300) {
        std::string out;
        for (;;) {
            struct pollfd pfd{sv_[1], POLLIN, 0};
            if (poll(&pfd, 1, timeout_ms) <= 0) break;
            char buf[4096];
            ssize_t n = recv(sv_[1], buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
            timeout_ms = 50;  // first read waits, the rest just drain
        }
        return out;
    }

    Server server_;
    int sv_[2] = {-1, -1};
    std::filesystem::path dir_;

    bool done_ = false;
    bool completed_ = false;
    bool errored_ = false;
    size_t bytes_sent_ = 0;
    int last_error_ = 0;
};

// ---------------------------------------------------------------------------
// FIFO: the ring-thread freeze
// ---------------------------------------------------------------------------

TEST_F(HttpFileJobOpenTest, FifoDoesNotBlockTheRingThread) {
    const std::string path = makeFifo("pipe");

    HTTPFileJob* job = makeJob(path);
    ASSERT_NE(job, nullptr);

    ASSERT_TRUE(startWithoutBlocking(job, path))
        << "start() parked inside open() waiting for a writer on the FIFO. On a "
           "real server that is the ring thread, so every connection on that core "
           "is frozen with it until someone opens the write end.";

    ASSERT_TRUE(pumpUntilDone());

    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 404"), std::string::npos)
        << "a FIFO is not a servable file; the client must get an error. Got:\n" << sent;
    EXPECT_EQ(sent.find("HTTP/1.1 200"), std::string::npos)
        << "no success status may be sent for a non-regular file";
}

// ---------------------------------------------------------------------------
// Directory: headers go out before the failure is discovered
// ---------------------------------------------------------------------------

TEST_F(HttpFileJobOpenTest, DirectoryIsRejectedBeforeAnyHeadersAreSent) {
    const std::string path = makeDirectory("subdir");

    HTTPFileJob* job = makeJob(path);
    ASSERT_NE(job, nullptr);

    ASSERT_TRUE(startWithoutBlocking(job));
    ASSERT_TRUE(pumpUntilDone());

    const std::string sent = clientBytes();
    EXPECT_EQ(sent.find("HTTP/1.1 200"), std::string::npos)
        << "a directory opens and stats successfully, so the pre-fix code sent a "
           "200 and a Content-Length taken from the directory inode, then failed "
           "inside splice. The client sees a truncated success. Got:\n" << sent;
    EXPECT_NE(sent.find("HTTP/1.1 404"), std::string::npos)
        << "the rejection has to happen before headers, not after. Got:\n" << sent;
}

// ---------------------------------------------------------------------------
// Device node: opens whatever driver is behind it
// ---------------------------------------------------------------------------

TEST_F(HttpFileJobOpenTest, CharacterDeviceIsRejected) {
    struct stat st{};
    ASSERT_EQ(stat("/dev/zero", &st), 0);
    ASSERT_TRUE(S_ISCHR(st.st_mode));

    HTTPFileJob* job = makeJob("/dev/zero");
    ASSERT_NE(job, nullptr);

    ASSERT_TRUE(startWithoutBlocking(job));
    ASSERT_TRUE(pumpUntilDone());

    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 404"), std::string::npos)
        << "a character device is not a file to serve. Got:\n" << sent;
    EXPECT_EQ(sent.find("HTTP/1.1 200"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Regression guards: the ordinary paths must be untouched
// ---------------------------------------------------------------------------

TEST_F(HttpFileJobOpenTest, RegularFileIsStillServed) {
    const std::string body = "the quick brown fox";
    const std::string path = makeRegularFile("ok.txt", body);

    HTTPFileJob* job = makeJob(path);
    ASSERT_NE(job, nullptr);

    ASSERT_TRUE(startWithoutBlocking(job));
    ASSERT_TRUE(pumpUntilDone());

    EXPECT_TRUE(completed_) << "a plain regular file must still transfer";
    EXPECT_FALSE(errored_);

    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 200"), std::string::npos) << sent;
    EXPECT_NE(sent.find("content-length: " + std::to_string(body.size())),
              std::string::npos) << sent;
    EXPECT_NE(sent.find(body), std::string::npos) << sent;
}

TEST_F(HttpFileJobOpenTest, EmptyRegularFileIsStillServed) {
    // S_ISREG must not be confused with "has bytes" — the zero-length path is
    // its own fix (review item C4) and has to keep working.
    const std::string path = makeRegularFile("empty.txt", "");

    HTTPFileJob* job = makeJob(path);
    ASSERT_NE(job, nullptr);

    ASSERT_TRUE(startWithoutBlocking(job));
    ASSERT_TRUE(pumpUntilDone());

    EXPECT_TRUE(completed_);
    EXPECT_FALSE(errored_);

    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 200"), std::string::npos) << sent;
    EXPECT_NE(sent.find("content-length: 0"), std::string::npos) << sent;
}

TEST_F(HttpFileJobOpenTest, MissingFileIsStillNotFound) {
    HTTPFileJob* job = makeJob((dir_ / "no_such_file").string());
    ASSERT_NE(job, nullptr);

    ASSERT_TRUE(startWithoutBlocking(job));
    ASSERT_TRUE(pumpUntilDone());

    const std::string sent = clientBytes();
    EXPECT_NE(sent.find("HTTP/1.1 404"), std::string::npos) << sent;
}
