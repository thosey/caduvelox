#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <clocale>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "caduvelox/Server.hpp"
#include "caduvelox/http/HTTPFileJob.hpp"
#include "caduvelox/http/RangeRequest.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"

using namespace caduvelox;
namespace fs = std::filesystem;

/**
 * Range request support (review item L19).
 *
 * HTTPFileJob could already serve a byte range -- it took an offset and a
 * length, emitted 206 with a Content-Range, and after L13 refused an
 * out-of-bounds one with 416. Nothing ever gave it a range: the connection
 * constructed every file response with offset 0, and the Range request field
 * was read by nothing in the project. So a client asking to resume a download
 * got the whole file back under a 200, which is legal but makes resumption
 * impossible, and no response advertised Accept-Ranges, so a client had no way
 * to know that asking was pointless.
 *
 * Three layers are tested separately, because they fail separately:
 *   - parse_range()/resolve(), where all the arithmetic lives;
 *   - HTTPFileJob, where a range meets a real file size;
 *   - a live server, where the Range field has to survive the trip from the
 *     request to the job -- the part that was missing outright.
 */
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(RangeParse, NoFieldIsNotARangeRequest) {
    EXPECT_FALSE(http::parse_range("").has_value());
    EXPECT_FALSE(http::parse_range("   ").has_value());
}

TEST(RangeParse, FirstAndLastPosition) {
    auto spec = http::parse_range("bytes=10-19");
    ASSERT_TRUE(spec.has_value());
    EXPECT_FALSE(spec->suffix);
    EXPECT_EQ(spec->first, 10u);
    ASSERT_TRUE(spec->last.has_value());
    EXPECT_EQ(*spec->last, 19u);
}

TEST(RangeParse, OpenEndedRange) {
    auto spec = http::parse_range("bytes=10-");
    ASSERT_TRUE(spec.has_value());
    EXPECT_FALSE(spec->suffix);
    EXPECT_EQ(spec->first, 10u);
    EXPECT_FALSE(spec->last.has_value());
}

TEST(RangeParse, SuffixRange) {
    auto spec = http::parse_range("bytes=-500");
    ASSERT_TRUE(spec.has_value());
    EXPECT_TRUE(spec->suffix);
    EXPECT_EQ(spec->first, 500u) << "for a suffix range `first` is a byte count, not a position";
    EXPECT_FALSE(spec->last.has_value());
}

TEST(RangeParse, UnitIsCaseInsensitiveAndOwsIsAllowed) {
    EXPECT_TRUE(http::parse_range("BYTES=0-1").has_value());
    EXPECT_TRUE(http::parse_range("  bytes =  0-1  ").has_value());
}

TEST(RangeParse, AnotherRangeUnitIsIgnored) {
    // RFC 9110 defines only `bytes`; a server that does not know a unit must
    // serve the whole representation rather than guess at byte semantics.
    EXPECT_FALSE(http::parse_range("items=0-1").has_value());
    EXPECT_FALSE(http::parse_range("seconds=0-10").has_value());
}

TEST(RangeParse, MultipleRangesAreIgnored) {
    // Answering only the first part while claiming to answer the request would
    // silently truncate; RFC 9110 section 14.2 permits ignoring the field.
    EXPECT_FALSE(http::parse_range("bytes=0-99,200-299").has_value());
    EXPECT_FALSE(http::parse_range("bytes=0-99, 200-").has_value());
    EXPECT_FALSE(http::parse_range("bytes=-50,-60").has_value());
}

TEST(RangeParse, MalformedFieldsAreIgnoredRatherThanRefused) {
    // Every one of these must come back "no range", because a 400 here would
    // break a request the server can answer perfectly well.
    EXPECT_FALSE(http::parse_range("bytes").has_value());
    EXPECT_FALSE(http::parse_range("bytes=").has_value());
    EXPECT_FALSE(http::parse_range("bytes=-").has_value());
    EXPECT_FALSE(http::parse_range("bytes=abc-def").has_value());
    EXPECT_FALSE(http::parse_range("bytes=1-2x").has_value());
    EXPECT_FALSE(http::parse_range("bytes=0x10-").has_value());
    EXPECT_FALSE(http::parse_range("0-10").has_value());
}

TEST(RangeParse, LastPositionBelowFirstIsInvalidNotUnsatisfiable) {
    // An inverted range is a malformed field, so it is ignored (whole file),
    // not answered with 416.
    EXPECT_FALSE(http::parse_range("bytes=99-10").has_value());
}

TEST(RangeParse, OverflowingPositionsAreRefused) {
    // 30 nines does not fit in a uint64_t. Wrapping would turn "far past the
    // end of the file" into a small offset inside it and serve the wrong bytes
    // under a 206.
    EXPECT_FALSE(http::parse_range("bytes=999999999999999999999999999999-").has_value());
    EXPECT_FALSE(http::parse_range("bytes=-999999999999999999999999999999").has_value());
    // The largest value that does fit still parses.
    auto spec = http::parse_range("bytes=18446744073709551615-");
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->first, UINT64_MAX);
}

// ---------------------------------------------------------------------------
// Resolution against a known size
// ---------------------------------------------------------------------------

std::optional<http::ResolvedRange> resolve(const char* field, uint64_t size) {
    auto spec = http::parse_range(field);
    if (!spec.has_value()) return std::nullopt;
    return spec->resolve(size);
}

TEST(RangeResolve, FirstAndLastPositionAreInclusive) {
    auto r = resolve("bytes=0-499", 10000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->offset, 0u);
    EXPECT_EQ(r->length, 500u) << "0-499 inclusive is 500 bytes";
}

TEST(RangeResolve, OpenEndedRangeRunsToTheEnd) {
    auto r = resolve("bytes=9500-", 10000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->offset, 9500u);
    EXPECT_EQ(r->length, 500u);
}

TEST(RangeResolve, SuffixRangeCountsBackFromTheEnd) {
    auto r = resolve("bytes=-500", 10000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->offset, 9500u);
    EXPECT_EQ(r->length, 500u);
}

TEST(RangeResolve, ALastPositionPastTheEndIsClamped) {
    // Not an error: RFC 9110 section 14.1.2 says a last-pos at or beyond the
    // current length means the remainder of the representation.
    auto r = resolve("bytes=5-99999", 10);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->offset, 5u);
    EXPECT_EQ(r->length, 5u);
}

TEST(RangeResolve, ASuffixLongerThanTheFileIsTheWholeFile) {
    auto r = resolve("bytes=-99999", 10);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->offset, 0u);
    EXPECT_EQ(r->length, 10u);
}

TEST(RangeResolve, AFirstPositionAtOrPastTheEndIsUnsatisfiable) {
    EXPECT_FALSE(resolve("bytes=10-", 10).has_value())
        << "byte 10 of a 10-byte file does not exist: positions are zero-based";
    EXPECT_FALSE(resolve("bytes=11-", 10).has_value());
    EXPECT_FALSE(resolve("bytes=10-20", 10).has_value());
}

TEST(RangeResolve, AZeroLengthSuffixIsUnsatisfiable) {
    // "bytes=-0" asks for the last nothing bytes. There is no such range, and
    // answering 200 would send a body the client did not ask for.
    EXPECT_FALSE(resolve("bytes=-0", 10).has_value());
}

TEST(RangeResolve, EveryRangeOverAnEmptyFileIsUnsatisfiable) {
    EXPECT_FALSE(resolve("bytes=0-", 0).has_value());
    EXPECT_FALSE(resolve("bytes=0-0", 0).has_value());
    EXPECT_FALSE(resolve("bytes=-1", 0).has_value());
}

TEST(RangeResolve, AWholeFileRangeStillResolves) {
    auto r = resolve("bytes=0-", 10);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->offset, 0u);
    EXPECT_EQ(r->length, 10u);
}

// ---------------------------------------------------------------------------
// HTTP dates
// ---------------------------------------------------------------------------

TEST(HttpDate, IsFormattedAsImfFixdate) {
    // The example from RFC 9110 section 5.6.7.
    EXPECT_EQ(http::format_http_date(784111777), "Sun, 06 Nov 1994 08:49:37 GMT");
    EXPECT_EQ(http::format_http_date(0), "Thu, 01 Jan 1970 00:00:00 GMT");
}

TEST(HttpDate, DayAndMonthNamesDoNotFollowTheLocale) {
    // strftime's %a and %b would emit localized names here, which no HTTP
    // client is required to parse.
    const char* previous = ::setlocale(LC_TIME, nullptr);
    const std::string saved = previous ? previous : "C";
    if (::setlocale(LC_TIME, "de_DE.UTF-8") == nullptr) {
        GTEST_SKIP() << "no de_DE.UTF-8 locale on this host";
    }
    EXPECT_EQ(http::format_http_date(784111777), "Sun, 06 Nov 1994 08:49:37 GMT");
    ::setlocale(LC_TIME, saved.c_str());
}

// ---------------------------------------------------------------------------
// At the job boundary: a range meets a real file
// ---------------------------------------------------------------------------

class RangeJobTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(128));
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_), 0);
        dir_ = fs::temp_directory_path() / ("cadu_l19_" + std::to_string(::getpid()));
        fs::create_directories(dir_);
        path_ = (dir_ / "payload.txt").string();
        std::ofstream(path_) << "0123456789";   // ten bytes
    }

    void TearDown() override {
        if (sv_[0] >= 0) ::close(sv_[0]);
        if (sv_[1] >= 0) ::close(sv_[1]);
        fs::remove_all(dir_);
    }

    // Serve path_ with the given Range field value, and return what reached
    // the client. An empty field means no Range field at all.
    std::string serve(const std::string& range_field) {
        auto* job = HTTPFileJob::createFromPool(
            sv_[0], path_, HttpResponse{},
            [this](int, size_t) { done_ = true; },
            [this](int, int) { done_ = true; });
        if (job == nullptr) return {};

        if (!range_field.empty()) {
            auto spec = http::parse_range(range_field);
            if (spec.has_value()) job->requestRange(*spec);
        }
        job->start(server_);
        if (!pumpUntilDone()) return "<timed out>";
        return clientBytes();
    }

    bool pumpUntilDone(int seconds = 3) {
        while (!done_) {
            struct __kernel_timespec ts{};
            ts.tv_sec = seconds;
            struct io_uring_cqe* cqe = nullptr;
            if (io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts) < 0) return false;
            auto* job = reinterpret_cast<IoJob*>(io_uring_cqe_get_data64(cqe));
            auto cleanup = job->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) (*cleanup)(job);
        }
        return true;
    }

    std::string clientBytes(int timeout_ms = 300) {
        std::string out;
        for (;;) {
            struct timeval tv{ .tv_sec = 0, .tv_usec = timeout_ms * 1000 };
            setsockopt(sv_[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[4096];
            ssize_t n = ::recv(sv_[1], buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
            timeout_ms = 50;
        }
        return out;
    }

    Server server_;
    int sv_[2] = {-1, -1};
    fs::path dir_;
    std::string path_;
    bool done_ = false;
};

TEST_F(RangeJobTest, AMiddleRangeSendsOnlyThoseBytes) {
    const std::string sent = serve("bytes=2-4");
    EXPECT_NE(sent.find("HTTP/1.1 206"), std::string::npos) << sent;
    EXPECT_NE(lower(sent).find("content-range: bytes 2-4/10"), std::string::npos) << sent;
    EXPECT_NE(lower(sent).find("content-length: 3"), std::string::npos) << sent;
    const size_t end = sent.find("\r\n\r\n");
    ASSERT_NE(end, std::string::npos) << sent;
    EXPECT_EQ(sent.substr(end + 4), "234") << sent;
}

TEST_F(RangeJobTest, AnOpenEndedRangeRunsToTheEnd) {
    const std::string sent = serve("bytes=7-");
    EXPECT_NE(sent.find("HTTP/1.1 206"), std::string::npos) << sent;
    EXPECT_NE(lower(sent).find("content-range: bytes 7-9/10"), std::string::npos) << sent;
    const size_t end = sent.find("\r\n\r\n");
    ASSERT_NE(end, std::string::npos) << sent;
    EXPECT_EQ(sent.substr(end + 4), "789") << sent;
}

TEST_F(RangeJobTest, ASuffixRangeSendsTheFinalBytes) {
    const std::string sent = serve("bytes=-3");
    EXPECT_NE(sent.find("HTTP/1.1 206"), std::string::npos) << sent;
    EXPECT_NE(lower(sent).find("content-range: bytes 7-9/10"), std::string::npos) << sent;
    const size_t end = sent.find("\r\n\r\n");
    ASSERT_NE(end, std::string::npos) << sent;
    EXPECT_EQ(sent.substr(end + 4), "789") << sent;
}

TEST_F(RangeJobTest, AnUnsatisfiableRangeIsRefusedWithTheRealLength) {
    const std::string sent = serve("bytes=20-30");
    EXPECT_NE(sent.find("HTTP/1.1 416"), std::string::npos) << sent;
    EXPECT_NE(lower(sent).find("content-range: bytes */10"), std::string::npos) << sent;
}

TEST_F(RangeJobTest, AWholeFileResponseAdvertisesRangeSupport) {
    const std::string sent = serve("");
    EXPECT_NE(sent.find("HTTP/1.1 200"), std::string::npos) << sent;
    EXPECT_NE(lower(sent).find("accept-ranges: bytes"), std::string::npos)
        << "without this field a client cannot know a range request is worth "
           "making. Got:\n" << sent;
    const size_t end = sent.find("\r\n\r\n");
    ASSERT_NE(end, std::string::npos) << sent;
    EXPECT_EQ(sent.substr(end + 4), "0123456789") << sent;
}

TEST_F(RangeJobTest, AFileResponseCarriesAValidator) {
    const std::string sent = serve("");
    const std::string head = lower(sent.substr(0, sent.find("\r\n\r\n")));
    ASSERT_NE(head.find("last-modified:"), std::string::npos)
        << "a range transfer that resumes across a change to the file needs "
           "something to validate against. Got:\n" << sent;
    EXPECT_NE(head.find(" gmt"), std::string::npos)
        << "Last-Modified must be an IMF-fixdate. Got:\n" << sent;
}

// ---------------------------------------------------------------------------
// End to end: the Range field has to reach the job
// ---------------------------------------------------------------------------

class RangeServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        port_ = BASE_PORT + counter_++;
        dir_ = fs::temp_directory_path() /
               ("cadu_l19_srv_" + std::to_string(::getpid()) + "_" + std::to_string(port_));
        fs::create_directories(dir_);
        std::ofstream(dir_ / "payload.txt") << kBody;

        ASSERT_TRUE(job_server_.init(256));
        http_server_ = std::make_unique<SingleRingHttpServer>(job_server_);
        http_server_->addRoute("GET", "^/payload$",
            [this](const HttpRequest&, HttpResponse& res) {
                res.sendFile((dir_ / "payload.txt").string());
            });
        // Same resource under POST, to show the field is ignored there.
        http_server_->addRoute("POST", "^/payload$",
            [this](const HttpRequest&, HttpResponse& res) {
                res.sendFile((dir_ / "payload.txt").string());
            });

        ASSERT_TRUE(http_server_->listen(port_, "127.0.0.1"));
        thread_ = std::thread([this]() { job_server_.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (http_server_) http_server_->stop();
        job_server_.stop();
        if (thread_.joinable()) thread_.join();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // One request on a fresh connection; returns the whole response.
    std::string get(const std::string& method, const std::string& range_field) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return {};
        }
        timeval tv{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string req = method + " /payload HTTP/1.1\r\nHost: localhost\r\n";
        if (!range_field.empty()) req += "Range: " + range_field + "\r\n";
        req += "Content-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);

        std::string out;
        for (;;) {
            char buf[4096];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);
        return out;
    }

    static std::string body(const std::string& response) {
        const size_t end = response.find("\r\n\r\n");
        return end == std::string::npos ? std::string{} : response.substr(end + 4);
    }

    static constexpr const char* kBody = "abcdefghijklmnopqrstuvwxyz";  // 26 bytes
    static constexpr uint16_t BASE_PORT = 19200;
    static int counter_;

    Server job_server_;
    std::unique_ptr<SingleRingHttpServer> http_server_;
    std::thread thread_;
    uint16_t port_ = 0;
    fs::path dir_;
};

int RangeServerTest::counter_ = 0;

TEST_F(RangeServerTest, ARangeFieldOnTheRequestProducesAPartialResponse) {
    // The whole point of L19: before this, the field was read by nothing and
    // every file response was the entire file under a 200.
    const std::string r = get("GET", "bytes=3-7");
    ASSERT_FALSE(r.empty());
    EXPECT_NE(r.find("HTTP/1.1 206"), std::string::npos)
        << "the Range field never reached the file job. Got:\n" << r;
    EXPECT_NE(lower(r).find("content-range: bytes 3-7/26"), std::string::npos) << r;
    EXPECT_EQ(body(r), "defgh") << r;
}

TEST_F(RangeServerTest, ASuffixRangeWorksEndToEnd) {
    const std::string r = get("GET", "bytes=-4");
    ASSERT_FALSE(r.empty());
    EXPECT_NE(r.find("HTTP/1.1 206"), std::string::npos) << r;
    EXPECT_NE(lower(r).find("content-range: bytes 22-25/26"), std::string::npos) << r;
    EXPECT_EQ(body(r), "wxyz") << r;
}

TEST_F(RangeServerTest, AnUnsatisfiableRangeIsA416) {
    const std::string r = get("GET", "bytes=100-200");
    ASSERT_FALSE(r.empty());
    EXPECT_NE(r.find("HTTP/1.1 416"), std::string::npos) << r;
    EXPECT_NE(lower(r).find("content-range: bytes */26"), std::string::npos) << r;
}

TEST_F(RangeServerTest, AMalformedRangeStillGetsTheWholeFile) {
    const std::string r = get("GET", "bytes=abc");
    ASSERT_FALSE(r.empty());
    EXPECT_NE(r.find("HTTP/1.1 200"), std::string::npos)
        << "a malformed Range field must be ignored, not refused. Got:\n" << r;
    EXPECT_EQ(body(r), kBody) << r;
}

TEST_F(RangeServerTest, AMultiRangeRequestGetsTheWholeFile) {
    const std::string r = get("GET", "bytes=0-3,10-13");
    ASSERT_FALSE(r.empty());
    EXPECT_NE(r.find("HTTP/1.1 200"), std::string::npos)
        << "this server does not do multipart/byteranges, so it must answer "
           "with the whole representation rather than one part. Got:\n" << r;
    EXPECT_EQ(body(r), kBody) << r;
    EXPECT_EQ(lower(r).find("content-range"), std::string::npos)
        << "a 200 is not a partial response and must not claim one. Got:\n" << r;
}

TEST_F(RangeServerTest, ARangeFieldIsIgnoredOnAMethodOtherThanGet) {
    // Range is defined for GET only. Truncating a non-GET reply to the asked
    // byte window would corrupt it.
    const std::string r = get("POST", "bytes=3-7");
    ASSERT_FALSE(r.empty());
    EXPECT_NE(r.find("HTTP/1.1 200"), std::string::npos) << r;
    EXPECT_EQ(body(r), kBody) << r;
}

TEST_F(RangeServerTest, AnOrdinaryResponseAdvertisesRangeSupport) {
    const std::string r = get("GET", "");
    ASSERT_FALSE(r.empty());
    EXPECT_NE(lower(r).find("accept-ranges: bytes"), std::string::npos) << r;
}

}  // namespace
