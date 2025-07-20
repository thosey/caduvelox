#include <gtest/gtest.h>
#include "caduvelox/http/HttpParser.hpp"

using namespace caduvelox;

class HttpParserUnitTest : public ::testing::Test {
protected:
    HttpRequest req;
    size_t consumed;

    void SetUp() override {
        req = HttpRequest{};
        consumed = 0;
    }
};

TEST_F(HttpParserUnitTest, ParsesSimpleGetRequest) {
    std::string request = 
        "GET /path HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    ASSERT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/path");
    EXPECT_EQ(req.version, "HTTP/1.1");
    EXPECT_EQ(req.headers.at("host"), "example.com");
    EXPECT_TRUE(req.body.empty());
    EXPECT_EQ(consumed, request.size());
}

TEST_F(HttpParserUnitTest, ParsesPostRequestWithBody) {
    std::string request = 
        "POST /api/data HTTP/1.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"key\":\"val\"}";

    ASSERT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(req.path, "/api/data");
    EXPECT_EQ(req.headers.at("content-type"), "application/json");
    EXPECT_EQ(req.headers.at("content-length"), "13");
    EXPECT_EQ(req.body, "{\"key\":\"val\"}");
    EXPECT_EQ(consumed, request.size());
}

TEST_F(HttpParserUnitTest, NormalizesHeaderNames) {
    std::string request = 
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: TestAgent/1.0\r\n"
        "Content-TYPE: text/plain\r\n"
        "\r\n";

    ASSERT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_EQ(req.headers.at("host"), "example.com");
    EXPECT_EQ(req.headers.at("user-agent"), "TestAgent/1.0");
    EXPECT_EQ(req.headers.at("content-type"), "text/plain");
}

TEST_F(HttpParserUnitTest, TrimsHeaderValues) {
    std::string request = 
        "GET / HTTP/1.1\r\n"
        "Host:   example.com   \r\n"
        "User-Agent:\tTestAgent/1.0\t\r\n"
        "\r\n";

    ASSERT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_EQ(req.headers.at("host"), "example.com");
    EXPECT_EQ(req.headers.at("user-agent"), "TestAgent/1.0");
}

TEST_F(HttpParserUnitTest, HandlesIncompleteRequest) {
    std::string incomplete = "GET /path HTTP/1.1\r\nHost: example.com\r\n";
    
    EXPECT_FALSE(HttpParser::parse_request(incomplete, req, consumed));
    EXPECT_EQ(consumed, 0);
}

TEST_F(HttpParserUnitTest, HandlesPartialBody) {
    std::string partial = 
        "POST /api HTTP/1.1\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "12345"; // Only 5 bytes of 10

    EXPECT_FALSE(HttpParser::parse_request(partial, req, consumed));
    EXPECT_EQ(consumed, 0);
}

TEST_F(HttpParserUnitTest, RejectsOversizeRequestLine) {
    std::string huge_path(HttpParser::MAX_REQUEST_LINE + 1, 'x');
    std::string request = "GET /" + huge_path + " HTTP/1.1\r\n\r\n";
    
    EXPECT_FALSE(HttpParser::parse_request(request, req, consumed));
}

TEST_F(HttpParserUnitTest, RejectsTooManyHeaders) {
    std::string request = "GET / HTTP/1.1\r\n";
    
    // Add more headers than the limit
    for (size_t i = 0; i <= HttpParser::MAX_HEADERS; ++i) {
        request += "Header" + std::to_string(i) + ": value\r\n";
    }
    request += "\r\n";
    
    EXPECT_FALSE(HttpParser::parse_request(request, req, consumed));
}

TEST_F(HttpParserUnitTest, RejectsOversizeHeaderLine) {
    std::string huge_value(HttpParser::MAX_HEADER_LINE + 1, 'x');
    std::string request = 
        "GET / HTTP/1.1\r\n"
        "BigHeader: " + huge_value + "\r\n"
        "\r\n";
    
    EXPECT_FALSE(HttpParser::parse_request(request, req, consumed));
}

TEST_F(HttpParserUnitTest, RejectsChunkedTransferEncoding) {
    std::string request = 
        "POST /data HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    
    EXPECT_FALSE(HttpParser::parse_request(request, req, consumed));
}

TEST_F(HttpParserUnitTest, AcceptsOtherTransferEncodings) {
    std::string request = 
        "POST /data HTTP/1.1\r\n"
        "Transfer-Encoding: gzip\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    
    EXPECT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_EQ(req.headers.at("transfer-encoding"), "gzip");
}

TEST_F(HttpParserUnitTest, HandlesEmptyBody) {
    std::string request = 
        "GET / HTTP/1.1\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    
    ASSERT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_TRUE(req.body.empty());
}

TEST_F(HttpParserUnitTest, HandlesNoContentLength) {
    std::string request = 
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";
    
    ASSERT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_TRUE(req.body.empty());
}

TEST_F(HttpParserUnitTest, ParsesMultipleHeadersCorrectly) {
    std::string request = 
        "PUT /resource HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "User-Agent: TestClient/1.0\r\n"
        "Accept: application/json\r\n"
        "Authorization: Bearer token123\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "{}";
    
    ASSERT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_EQ(req.method, "PUT");
    EXPECT_EQ(req.path, "/resource");
    EXPECT_EQ(req.headers.size(), 6);
    EXPECT_EQ(req.headers.at("host"), "api.example.com");
    EXPECT_EQ(req.headers.at("user-agent"), "TestClient/1.0");
    EXPECT_EQ(req.headers.at("accept"), "application/json");
    EXPECT_EQ(req.headers.at("authorization"), "Bearer token123");
    EXPECT_EQ(req.headers.at("content-type"), "application/json");
    EXPECT_EQ(req.headers.at("content-length"), "2");
    EXPECT_EQ(req.body, "{}");
}

TEST_F(HttpParserUnitTest, HandlesEdgeCaseMethod) {
    std::string request = 
        "OPTIONS * HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";
    
    ASSERT_TRUE(HttpParser::parse_request(request, req, consumed));
    EXPECT_EQ(req.method, "OPTIONS");
    EXPECT_EQ(req.path, "*");
    EXPECT_EQ(req.version, "HTTP/1.1");
}
