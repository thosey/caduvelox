#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace caduvelox;
namespace fs = std::filesystem;

/**
 * Percent-encoded paths, and the traversal that decoding them invites
 * (review item M12).
 *
 * Nothing decoded anything, which made the framework safe against %2e%2e by
 * accident and unable to serve an ordinary filename by the same accident.
 * Measured before the change, through a real server with a capture route over a
 * real directory:
 *
 *   /files/plain.txt        -> 200
 *   /files/a%20b.txt        -> 404   (the file "a b.txt" exists)
 *   /files/caf%C3%A9.txt    -> 404   (the file "café.txt" exists)
 *
 * A browser encodes both of those, so neither file could be fetched at all.
 *
 * Decoding removes the accidental protection, so the traversal cases below
 * matter more than the feature: they pass before the change because nothing
 * decodes, and they have to keep passing once something does. The dangerous one
 * is %2f -- decode before splitting on '/' and it becomes a separator, so
 * "..%2f..%2fsecret" turns into traversal that a ".." substring check never
 * sees. Segments are therefore split first and decoded individually.
 */
namespace {

class PathDecodingTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        root_ = fs::temp_directory_path() / ("cadu_m12_" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_ / "public");
        std::ofstream(root_ / "public" / "plain.txt") << "plain";
        std::ofstream(root_ / "public" / "a b.txt") << "spaces";
        std::ofstream(root_ / "public" / "café.txt") << "utf8";
        std::ofstream(root_ / "secret.txt") << "SECRET";

        port_ = BASE_PORT + counter_++;
        ASSERT_TRUE(job_server_.init(256));
        http_ = std::make_unique<SingleRingHttpServer>(job_server_);

        // The documented pattern: take the name from the capture, not from
        // req.path, and confine it to the docroot.
        const fs::path docroot = root_ / "public";
        http_->addRouteWithCaptures("GET", "^/files/(.+)$",
            [docroot](const HttpRequest&, HttpResponse& res, const std::smatch& m) {
                const fs::path candidate = docroot / m[1].str();
                const fs::path norm = fs::weakly_canonical(candidate);
                const fs::path base = fs::weakly_canonical(docroot);
                const auto rel = norm.lexically_relative(base);
                if (rel.empty() || rel.string().rfind("..", 0) == 0) {
                    res.setStatus(403, "Forbidden");
                    res.setBody("Forbidden");
                    return;
                }
                if (fs::exists(norm) && fs::is_regular_file(norm)) {
                    res.sendFile(norm.string());
                } else {
                    res.setStatus(404, "Not Found");
                    res.setBody("Not Found");
                }
            });

        ASSERT_TRUE(http_->listen(port_, "127.0.0.1"));
        ring_ = std::thread([this] { job_server_.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void TearDown() override {
        if (http_) http_->stop();
        job_server_.stop();
        if (ring_.joinable()) ring_.join();
        fs::remove_all(root_);
    }

    struct Reply { int status = 0; std::string body; std::string raw; };

    Reply get(const std::string& target) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port_);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
        timeval tv{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const std::string req =
            "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        EXPECT_EQ(::send(fd, req.data(), req.size(), MSG_NOSIGNAL),
                  static_cast<ssize_t>(req.size()));

        Reply out;
        char buf[8192];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) out.raw.append(buf, static_cast<size_t>(n));
        ::close(fd);

        if (out.raw.size() > 12) out.status = std::atoi(out.raw.substr(9, 3).c_str());
        const size_t end = out.raw.find("\r\n\r\n");
        if (end != std::string::npos) out.body = out.raw.substr(end + 4);
        return out;
    }

    static constexpr uint16_t BASE_PORT = 18400;
    static int counter_;

    fs::path root_;
    uint16_t port_ = 0;
    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_;
    std::thread ring_;
};

int PathDecodingTest::counter_ = 0;

// ---------------------------------------------------------------------------
// Encoded names must be reachable
// ---------------------------------------------------------------------------

TEST_F(PathDecodingTest, APlainNameIsServed) {
    const Reply r = get("/files/plain.txt");
    EXPECT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body, "plain");
}

TEST_F(PathDecodingTest, AnEncodedSpaceReachesItsFile) {
    const Reply r = get("/files/a%20b.txt");
    EXPECT_EQ(r.status, 200)
        << "a browser asking for the file 'a b.txt' sends %20; without decoding "
           "the file is unreachable. Got:\n" << r.raw;
    EXPECT_EQ(r.body, "spaces");
}

TEST_F(PathDecodingTest, EncodedUtf8ReachesItsFile) {
    const Reply r = get("/files/caf%C3%A9.txt");
    EXPECT_EQ(r.status, 200) << r.raw;
    EXPECT_EQ(r.body, "utf8");
}

TEST_F(PathDecodingTest, APlusSignIsNotASpaceInAPath) {
    // '+' means space in a query string, never in a path (RFC 3986).
    const Reply r = get("/files/a+b.txt");
    EXPECT_NE(r.status, 200)
        << "there is no file called 'a b.txt' reachable as 'a+b.txt'. Got:\n" << r.raw;
}

// ---------------------------------------------------------------------------
// Traversal: passes before decoding exists, must pass after
// ---------------------------------------------------------------------------

TEST_F(PathDecodingTest, EncodedDotDotCannotEscapeTheDocroot) {
    for (const char* target : {"/files/%2e%2e/secret.txt",
                               "/files/%2E%2E/secret.txt",
                               "/files/..%2fsecret.txt",
                               "/files/..%2Fsecret.txt",
                               "/files/%2e%2e%2fsecret.txt",
                               "/files/a/..%2f..%2fsecret.txt"}) {
        const Reply r = get(target);
        EXPECT_NE(r.status, 200) << target << " was served. Got:\n" << r.raw;
        EXPECT_EQ(r.body.find("SECRET"), std::string::npos)
            << target << " leaked the file outside the docroot";
    }
}

TEST_F(PathDecodingTest, DoubleEncodingDoesNotEscapeEither) {
    // %252e decodes once to %2e. Decoding twice would turn this into "..".
    for (const char* target : {"/files/%252e%252e/secret.txt",
                               "/files/%252e%252e%252fsecret.txt"}) {
        const Reply r = get(target);
        EXPECT_NE(r.status, 200) << target << " was served. Got:\n" << r.raw;
        EXPECT_EQ(r.body.find("SECRET"), std::string::npos) << target;
    }
}

TEST_F(PathDecodingTest, AnEncodedSeparatorIsNotAcceptedInsideASegment) {
    // Decoding %2f into '/' would silently create a path segment boundary.
    const Reply r = get("/files/sub%2fplain.txt");
    EXPECT_NE(r.status, 200) << r.raw;
}

TEST_F(PathDecodingTest, AnEncodedNulIsRefused) {
    const Reply r = get("/files/plain.txt%00.png");
    EXPECT_NE(r.status, 200) << r.raw;
    EXPECT_EQ(r.body.find("plain"), std::string::npos)
        << "a NUL must not truncate the name back to a servable file";
}

TEST_F(PathDecodingTest, MalformedEscapesAreRefused) {
    for (const char* target : {"/files/a%2.txt", "/files/a%zz.txt", "/files/a%.txt"}) {
        const Reply r = get(target);
        EXPECT_NE(r.status, 200) << target << " got:\n" << r.raw;
    }
}

}  // namespace
