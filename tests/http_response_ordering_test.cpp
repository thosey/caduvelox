#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <optional>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "caduvelox/Server.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"

using namespace caduvelox;

/**
 * Response ordering under HTTP pipelining — review item C6.
 *
 * processHttpRequests() used to parse *every* complete request sitting in the
 * buffer and issue one WriteJob per response. Those SQEs are independent, and
 * io_uring guarantees nothing about the order in which it executes them: a write
 * that cannot complete inline is punted to an async worker while the next one
 * proceeds, so response bytes can interleave and corrupt the HTTP stream.
 *
 * The fix is strict per-connection serialization: one response in flight at a
 * time, and request k+1 is not even parsed until response k is on the wire.
 *
 * The race itself is not deterministically reproducible from a test — it needs
 * the kernel to punt one specific write. What IS deterministic is the
 * serialization that removes it, and that is what these tests pin. See
 * OnlyOneResponseIsInFlightWhileTheClientRefusesToRead, which fails against the
 * pre-fix dispatch loop.
 */
namespace {

struct ParsedResponse {
    int         status_code = 0;
    size_t      content_length = 0;
    size_t      total_bytes = 0;   // headers + body, for advancing the read buffer
    std::string body;
};

// Parse one response off the front of `buffer`. nullopt means "need more data".
std::optional<ParsedResponse> tryParseOne(const std::string& buffer) {
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) return std::nullopt;

    ParsedResponse res{};
    if (buffer.size() < 12) return std::nullopt;
    res.status_code = std::atoi(buffer.substr(9, 3).c_str());

    std::string headers = buffer.substr(0, header_end + 4);
    std::string lower = headers;
    for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

    const std::string key = "content-length:";
    size_t pos = lower.find(key);
    if (pos == std::string::npos) return std::nullopt;
    size_t start = pos + key.size();
    while (start < headers.size() && (headers[start] == ' ' || headers[start] == '\t')) ++start;
    size_t end = headers.find("\r\n", start);
    if (end == std::string::npos) return std::nullopt;
    res.content_length = static_cast<size_t>(std::strtoul(headers.substr(start, end - start).c_str(), nullptr, 10));

    size_t body_start = header_end + 4;
    if (buffer.size() - body_start < res.content_length) return std::nullopt;

    res.body = buffer.substr(body_start, res.content_length);
    res.total_bytes = body_start + res.content_length;
    return res;
}

// Bodies are "<index>:<padding>", so a response identifies its request even
// after the padding makes the write large enough to be interesting.
int indexFromBody(const std::string& body) {
    size_t colon = body.find(':');
    if (colon == std::string::npos) return -1;
    return std::atoi(body.substr(0, colon).c_str());
}

} // namespace

class HttpResponseOrderingTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);

        test_port_ = BASE_PORT + test_counter_++;

        ASSERT_TRUE(job_server_.init(256));
        http_server_ = std::make_unique<SingleRingHttpServer>(job_server_);

        // /seq/<n>[?pad=<bytes>] — echoes n, padded to the requested size.
        http_server_->addRoute("GET", "^/seq/.*$",
            [this](const HttpRequest& req, HttpResponse& res) {
                dispatch_count_.fetch_add(1, std::memory_order_acq_rel);

                std::string path = req.path;
                size_t slash = path.rfind('/');
                std::string index = (slash == std::string::npos) ? "0" : path.substr(slash + 1);
                {
                    std::lock_guard<std::mutex> lock(order_mutex_);
                    dispatch_order_.push_back(std::atoi(index.c_str()));
                }

                res.status_code = 200;
                res.headers["content-type"] = "text/plain";
                res.body = index + ":" + std::string(pad_bytes_, 'x');
            });

        ASSERT_TRUE(http_server_->listen(test_port_, "127.0.0.1"));

        server_thread_ = std::thread([this]() { job_server_.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (client_fd_ >= 0) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
        if (http_server_) {
            http_server_->stop();
        }
        job_server_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    int connectClient(int recv_buf_bytes = 0) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);

        if (recv_buf_bytes > 0) {
            // Must be set before connect() so it is used for window advertisement.
            EXPECT_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recv_buf_bytes, sizeof(recv_buf_bytes)), 0);
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(test_port_);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

        timeval tv{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        client_fd_ = fd;
        return fd;
    }

    // Send `count` pipelined GETs in a single write, so the server sees them
    // together and the old code would dispatch them all in one loop.
    static void sendPipelined(int fd, int count) {
        std::string all;
        for (int i = 0; i < count; ++i) {
            all += "GET /seq/" + std::to_string(i) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        }
        size_t off = 0;
        while (off < all.size()) {
            ssize_t n = ::send(fd, all.data() + off, all.size() - off, 0);
            ASSERT_GT(n, 0) << "send failed: " << strerror(errno);
            off += static_cast<size_t>(n);
        }
    }

    // Read and parse exactly `count` responses, returning their body indices in
    // arrival order.
    static std::vector<int> readResponses(int fd, int count) {
        std::vector<int> indices;
        std::string buf;
        char tmp[16384];

        while (static_cast<int>(indices.size()) < count) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                ADD_FAILURE() << "recv failed after " << indices.size()
                              << " responses: " << strerror(errno);
                return indices;
            }
            if (n == 0) {
                ADD_FAILURE() << "connection closed after " << indices.size()
                              << " of " << count << " responses";
                return indices;
            }
            buf.append(tmp, tmp + n);

            while (auto parsed = tryParseOne(buf)) {
                EXPECT_EQ(parsed->status_code, 200);
                indices.push_back(indexFromBody(parsed->body));
                buf.erase(0, parsed->total_bytes);
                if (static_cast<int>(indices.size()) == count) break;
            }
        }
        return indices;
    }

    static constexpr uint16_t BASE_PORT = 9700;
    static int test_counter_;

    Server                              job_server_;
    std::unique_ptr<SingleRingHttpServer> http_server_;
    std::thread                         server_thread_;
    uint16_t                            test_port_ = 0;
    int                                 client_fd_ = -1;

    std::atomic<int>                    dispatch_count_{0};
    std::mutex                          order_mutex_;
    std::vector<int>                    dispatch_order_;
    size_t                              pad_bytes_ = 8;   // per-test knob
};

int HttpResponseOrderingTest::test_counter_ = 0;

// --- The mechanism: one response in flight at a time ---

/**
 * This is the test that distinguishes the fix from the bug.
 *
 * The client sets a tiny receive buffer and then does not read at all. Once the
 * socket's send path is full, a response can no longer complete, so under strict
 * serialization the server MUST stall: it will not dispatch the next pipelined
 * request. The pre-fix loop had no such dependency — it parsed and dispatched
 * all 64 requests and submitted all 64 writes back to back, which is exactly the
 * unordered-SQE situation C6 describes.
 */
TEST_F(HttpResponseOrderingTest, OnlyOneResponseIsInFlightWhileTheClientRefusesToRead) {
    constexpr int kRequests = 64;
    pad_bytes_ = 256 * 1024;   // 64 * 256 KB = 16 MB, far past any socket buffer

    int fd = connectClient(/*recv_buf_bytes=*/2048);
    sendPipelined(fd, kRequests);

    // Give the server every chance to run ahead. It cannot, unless responses are
    // being issued without waiting for the previous one to complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(750));

    int dispatched = dispatch_count_.load(std::memory_order_acquire);

    EXPECT_LT(dispatched, kRequests)
        << "all " << kRequests << " pipelined requests were dispatched without a single "
        << "response completing — responses are not serialized";

    // Requests are still handled in the order they were received.
    {
        std::lock_guard<std::mutex> lock(order_mutex_);
        for (size_t i = 0; i < dispatch_order_.size(); ++i) {
            EXPECT_EQ(dispatch_order_[i], static_cast<int>(i)) << "dispatch out of order at " << i;
        }
    }
}

// --- Functional ordering guards ---

TEST_F(HttpResponseOrderingTest, PipelinedResponsesArriveInRequestOrder) {
    constexpr int kRequests = 32;
    pad_bytes_ = 64;

    int fd = connectClient();
    sendPipelined(fd, kRequests);

    std::vector<int> indices = readResponses(fd, kRequests);

    ASSERT_EQ(indices.size(), static_cast<size_t>(kRequests));
    for (int i = 0; i < kRequests; ++i) {
        EXPECT_EQ(indices[i], i) << "response " << i << " arrived out of order";
    }
}

/**
 * Large responses are the ones that actually risk being punted to an io_uring
 * async worker, which is the mechanism behind C6. 8 x 512 KB will not complete
 * inline on a socket whose peer is reading slowly.
 */
TEST_F(HttpResponseOrderingTest, LargePipelinedResponsesStayOrdered) {
    constexpr int kRequests = 8;
    pad_bytes_ = 512 * 1024;

    int fd = connectClient(/*recv_buf_bytes=*/16384);
    sendPipelined(fd, kRequests);

    std::vector<int> indices = readResponses(fd, kRequests);

    ASSERT_EQ(indices.size(), static_cast<size_t>(kRequests));
    for (int i = 0; i < kRequests; ++i) {
        EXPECT_EQ(indices[i], i) << "large response " << i << " arrived out of order";
    }
}

/**
 * The buffer can hold more requests than one recv delivered. Sending in three
 * separate bursts exercises the path where onResponseComplete() re-enters
 * processHttpRequests() with data that arrived while a response was in flight.
 */
TEST_F(HttpResponseOrderingTest, RequestsArrivingMidResponseAreStillOrdered) {
    constexpr int kBursts = 3;
    constexpr int kPerBurst = 8;
    pad_bytes_ = 32 * 1024;

    int fd = connectClient(/*recv_buf_bytes=*/16384);

    std::string all;
    for (int i = 0; i < kBursts * kPerBurst; ++i) {
        all += "GET /seq/" + std::to_string(i) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    }
    // Send in bursts, without reading in between, so requests pile up behind an
    // in-flight response.
    size_t chunk = all.size() / kBursts;
    size_t off = 0;
    for (int b = 0; b < kBursts; ++b) {
        size_t len = (b == kBursts - 1) ? all.size() - off : chunk;
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd, all.data() + off + sent, len - sent, 0);
            ASSERT_GT(n, 0) << "send failed: " << strerror(errno);
            sent += static_cast<size_t>(n);
        }
        off += len;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::vector<int> indices = readResponses(fd, kBursts * kPerBurst);

    ASSERT_EQ(indices.size(), static_cast<size_t>(kBursts * kPerBurst));
    for (int i = 0; i < kBursts * kPerBurst; ++i) {
        EXPECT_EQ(indices[i], i) << "response " << i << " arrived out of order";
    }
}

/**
 * Serialization must not break the ordinary one-request-at-a-time case, and the
 * connection must stay alive across all of them.
 */
TEST_F(HttpResponseOrderingTest, SequentialKeepAliveRequestsStillWork) {
    pad_bytes_ = 16;

    int fd = connectClient();

    for (int i = 0; i < 10; ++i) {
        std::string req = "GET /seq/" + std::to_string(i) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ASSERT_EQ(::send(fd, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));

        std::vector<int> indices = readResponses(fd, 1);
        ASSERT_EQ(indices.size(), 1u);
        EXPECT_EQ(indices[0], i);
    }

    EXPECT_EQ(dispatch_count_.load(std::memory_order_acquire), 10);
}
