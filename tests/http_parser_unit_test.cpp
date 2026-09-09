#include <gtest/gtest.h>
#include "caduvelox/http/HttpParser.hpp"

using namespace caduvelox;
using PR = HttpParser::ParseResult;

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

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
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

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
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

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
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

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
    EXPECT_EQ(req.headers.at("host"), "example.com");
    EXPECT_EQ(req.headers.at("user-agent"), "TestAgent/1.0");
}

TEST_F(HttpParserUnitTest, HandlesIncompleteRequest) {
    std::string incomplete = "GET /path HTTP/1.1\r\nHost: example.com\r\n";

    EXPECT_EQ(HttpParser::parse_request(incomplete, req, consumed), PR::Incomplete);
    EXPECT_EQ(consumed, 0);
}

TEST_F(HttpParserUnitTest, HandlesPartialBody) {
    std::string partial =
        "POST /api HTTP/1.1\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "12345"; // Only 5 bytes of 10

    EXPECT_EQ(HttpParser::parse_request(partial, req, consumed), PR::Incomplete);
    EXPECT_EQ(consumed, 0);
}

TEST_F(HttpParserUnitTest, RejectsOversizeRequestLine) {
    std::string huge_path(HttpParser::MAX_REQUEST_LINE + 1, 'x');
    std::string request = "GET /" + huge_path + " HTTP/1.1\r\n\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsTooManyHeaders) {
    std::string request = "GET / HTTP/1.1\r\n";

    for (size_t i = 0; i <= HttpParser::MAX_HEADERS; ++i) {
        request += "Header" + std::to_string(i) + ": value\r\n";
    }
    request += "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsOversizeHeaderLine) {
    std::string huge_value(HttpParser::MAX_HEADER_LINE + 1, 'x');
    std::string request =
        "GET / HTTP/1.1\r\n"
        "BigHeader: " + huge_value + "\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

// ---------------------------------------------------------------------------
// Transfer-Encoding (review item H7)
//
// This server implements no transfer codings at all, so any request carrying
// the field is one it cannot frame. RFC 9112 section 6.1 also requires chunked
// to be the final coding applied to a request, which makes a lone
// "Transfer-Encoding: gzip" malformed in its own right.
//
// The parser used to reject only values containing "chunked" -- the classic
// CL.TE desync -- and store anything else. Storing it is the problem: nothing
// in this server ever reads the field, so the body was framed by
// Content-Length while a front end that honoured the coding framed it
// differently. That disagreement is the whole desync, and chunked is only its
// best-known instance.
// ---------------------------------------------------------------------------

TEST_F(HttpParserUnitTest, RejectsChunkedTransferEncoding) {
    std::string request =
        "POST /data HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

// Behaviour change: this was AcceptsOtherTransferEncodings, which asserted that
// a non-chunked coding parsed successfully and kept its value.
TEST_F(HttpParserUnitTest, RejectsOtherTransferEncodings) {
    std::string request =
        "POST /data HTTP/1.1\r\n"
        "Transfer-Encoding: gzip\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest)
        << "a transfer coding this server cannot apply leaves the body framed "
           "by Content-Length here and by the coding upstream";
}

// The framing disagreement itself, with a body to disagree about: a front end
// honouring the coding reads the body one way, this server reads five bytes.
TEST_F(HttpParserUnitTest, RejectsTransferEncodingCombinedWithContentLength) {
    std::string request =
        "POST /data HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: gzip\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "helloGET /admin HTTP/1.1\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest)
        << "whatever the two ends disagree about becomes the head of the next "
           "request for one of them, and the attacker writes that request";
}

// RFC 9112 section 6.1: identity is not a transfer coding a request may carry.
TEST_F(HttpParserUnitTest, RejectsIdentityTransferEncoding) {
    std::string request =
        "POST /data HTTP/1.1\r\n"
        "Transfer-Encoding: identity\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

// An empty value used to slip through: there is no "chunked" in "" to find.
TEST_F(HttpParserUnitTest, RejectsEmptyTransferEncoding) {
    std::string request =
        "POST /data HTTP/1.1\r\n"
        "Transfer-Encoding: \r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

// Guard: the field is matched by name, not by substring. A header that merely
// contains the name is an ordinary header and must still be accepted.
TEST_F(HttpParserUnitTest, DoesNotRejectHeadersNamedAfterTransferEncoding) {
    std::string request =
        "GET / HTTP/1.1\r\n"
        "X-Transfer-Encoding: gzip\r\n"
        "\r\n";

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
    EXPECT_EQ(req.headers.at("x-transfer-encoding"), "gzip");
}

// Guard: TE is a different field (RFC 9110 section 10.1.4) -- it advertises what
// codings the client would accept in the response, and frames nothing.
TEST_F(HttpParserUnitTest, StillAcceptsTheTeHeader) {
    std::string request =
        "GET / HTTP/1.1\r\n"
        "TE: gzip\r\n"
        "\r\n";

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
    EXPECT_EQ(req.headers.at("te"), "gzip");
}

TEST_F(HttpParserUnitTest, HandlesEmptyBody) {
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
    EXPECT_TRUE(req.body.empty());
}

TEST_F(HttpParserUnitTest, HandlesNoContentLength) {
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
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

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
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

TEST_F(HttpParserUnitTest, RejectsTabSeparatedRequestLine) {
    // HTTP spec requires SP (0x20) only — tabs are not valid separators
    std::string request = "GET\t/path\tHTTP/1.1\r\n\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, HandlesEdgeCaseMethod) {
    std::string request =
        "OPTIONS * HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
    EXPECT_EQ(req.method, "OPTIONS");
    EXPECT_EQ(req.path, "*");
    EXPECT_EQ(req.version, "HTTP/1.1");
}

// --- Request framing and field syntax (review item H1) ---
//
// Everything below is one class of bug: input this parser once accepted, or
// silently reinterpreted, that a proxy sitting in front of the server would read
// differently. When the two disagree about where a request ends, the bytes one
// of them considers leftover become the start of a request the other one
// executes -- and the attacker chose those bytes. Each test names the primitive
// it closes.

TEST_F(HttpParserUnitTest, RejectsNonNumericContentLength) {
    // The original bug. strtoul("abc") returns 0, so the parser framed the
    // request at the end of the headers and left the 5 body bytes in the buffer.
    // The connection loop then parsed "GET /admin ..." as a second request that
    // the client never framed as one.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: abc\r\n"
        "\r\n"
        "GET /admin HTTP/1.1\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
    EXPECT_EQ(consumed, 0u);
}

TEST_F(HttpParserUnitTest, RejectsContentLengthWithTrailingJunk) {
    // strtoul stops at the first non-digit and reports success for the prefix,
    // so "5x" used to frame a 5-byte body while a stricter recipient rejected
    // the message outright.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "Content-Length: 5x\r\n"
        "\r\n"
        "hello";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsContentLengthThatIsNotAPlainDigitString) {
    // strtoul honoured an explicit sign and understood other bases. None of
    // these are 1*DIGIT, and "-5" in particular used to wrap to a huge value
    // that only the 1 GiB cap happened to catch. The empty value is in here too:
    // "Content-Length:" with nothing after it is not a length.
    for (const char* value : {"+5", "-5", "0x5", "5 5", "", " "}) {
        SetUp();
        std::string request =
            std::string("POST /api HTTP/1.1\r\nContent-Length: ") + value + "\r\n"
            "\r\n"
            "hello";

        EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest)
            << "Content-Length: '" << value << "' must be rejected";
    }
}

TEST_F(HttpParserUnitTest, AcceptsContentLengthWithSurroundingWhitespace) {
    // The counterpart to the test above, so strictness does not swallow legal
    // input: OWS on either side of a field value is not part of the value, and
    // trim_header_value() removes it before the digits-only check runs.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "Content-Length:   5  \r\n"
        "\r\n"
        "hello";

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
    EXPECT_EQ(req.body, "hello");
}

TEST_F(HttpParserUnitTest, RejectsContentLengthThatOverflows) {
    // Forty digits. The old post-hoc "> 1 GiB" check ran on an already-wrapped
    // strtoul result, so a long enough digit string could land back inside the
    // accepted range.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "Content-Length: " + std::string(40, '9') + "\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, AcceptsContentLengthAtTheCap) {
    // The boundary itself is legal; only the body is missing, so this is
    // Incomplete rather than BadRequest. Guards against an off-by-one in the
    // overflow-safe accumulate that would reject the largest allowed value.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "Content-Length: " + std::to_string(HttpParser::MAX_CONTENT_LENGTH) + "\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::Incomplete);

    SetUp();
    std::string over =
        "POST /api HTTP/1.1\r\n"
        "Content-Length: " + std::to_string(HttpParser::MAX_CONTENT_LENGTH + 1) + "\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(over, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsMismatchedDuplicateContentLength) {
    // CL.CL desync. The headers went into an unordered_map, so the second value
    // silently overwrote the first and this server framed 5 bytes while a
    // front-end that kept the first value framed 0.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "Content-Length: 0\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, AcceptsIdenticalDuplicateContentLength) {
    // RFC 9110 section 8.6 only invalidates the message when the values differ.
    // Repeated identical values are unambiguous, so there is no boundary to
    // disagree about.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
    EXPECT_EQ(req.body, "hello");
    EXPECT_EQ(consumed, request.size());
}

TEST_F(HttpParserUnitTest, RejectsFieldLineWithoutColon) {
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "NotAHeader\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsWhitespaceBeforeColon) {
    // "Content-Length : 5" produced the field name "content-length " -- with the
    // trailing space, since only values were trimmed. The lookup for
    // "content-length" then missed entirely and the body was framed as zero
    // bytes, while any recipient that trims the name reads a 5-byte body.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "Content-Length : 5\r\n"
        "\r\n"
        "hello";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsObsFoldContinuationLine) {
    // Deprecated line folding. RFC 9112 section 5.2 requires a server to reject
    // it in a request rather than guess at the unfolded value.
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "  continued\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsEmptyFieldName) {
    std::string request =
        "GET / HTTP/1.1\r\n"
        ": value\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsNonTokenFieldName) {
    // A field name is a token. Separators like "(" or "@" are not token
    // characters and their handling varies between implementations.
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Bad(Name): value\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsBareLineFeedInsideFieldValue) {
    // Header injection by way of line-ending disagreement. This parser splits on
    // CRLF, so the bare LF used to stay inside the User-Agent value; a recipient
    // that splits on LF alone reads a Content-Length header this server does not
    // see, and the two frame the message differently.
    std::string request =
        "POST /api HTTP/1.1\r\n"
        "User-Agent: evil\nContent-Length: 5\r\n"
        "\r\n"
        "hello";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsControlCharacterInsideFieldValue) {
    std::string request =
        "GET / HTTP/1.1\r\n"
        "X-Thing: va\x01lue\r\n"
        "\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, AcceptsInteriorWhitespaceInFieldValue) {
    // Strictness must not cost legitimate values: SP and HTAB are valid inside a
    // field value, and only the surrounding ones get trimmed.
    std::string request =
        "GET / HTTP/1.1\r\n"
        "User-Agent:  Mozilla/5.0 (X11; Linux)\t \r\n"
        "\r\n";

    ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success);
    EXPECT_EQ(req.headers.at("user-agent"), "Mozilla/5.0 (X11; Linux)");
}

TEST_F(HttpParserUnitTest, RejectsNonTokenMethod) {
    std::string request = "G@T / HTTP/1.1\r\nHost: example.com\r\n\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsControlCharacterInRequestTarget) {
    std::string request = "GET /pa\tth HTTP/1.1\r\nHost: example.com\r\n\r\n";

    EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest);
}

TEST_F(HttpParserUnitTest, RejectsMalformedHttpVersion) {
    // "HTTP/9.1.1" matters specifically: shouldKeepAlive() tests
    // version.find("1.1"), so an unvalidated version string is a way to turn on
    // connection reuse from a version the server does not actually speak.
    for (const char* version : {"HTTP/9.1.1", "HTTP/1", "HTTP/1.", "HTTPS/1.1",
                                "http/1.1", "HTTP/1.1x", "1.1"}) {
        SetUp();
        std::string request = std::string("GET / ") + version + "\r\nHost: h\r\n\r\n";

        EXPECT_EQ(HttpParser::parse_request(request, req, consumed), PR::BadRequest)
            << "version '" << version << "' must be rejected";
    }
}

TEST_F(HttpParserUnitTest, AcceptsSupportedHttpVersions) {
    for (const char* version : {"HTTP/1.0", "HTTP/1.1"}) {
        SetUp();
        std::string request = std::string("GET / ") + version + "\r\nHost: h\r\n\r\n";

        ASSERT_EQ(HttpParser::parse_request(request, req, consumed), PR::Success)
            << "version '" << version << "' must be accepted";
        EXPECT_EQ(req.version, version);
    }
}
