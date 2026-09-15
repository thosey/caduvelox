#include <gtest/gtest.h>
#include "caduvelox/http/HttpResponse.hpp"
#include "caduvelox/http/HttpResponseWriter.hpp"
#include <string>
#include <vector>

using namespace caduvelox;

/**
 * build_response_head() in isolation (review item M7). The wire-level behaviour
 * is covered in response_header_framing_test; these pin the rules themselves.
 */
namespace {

TEST(ResponseHeadWriter, WritesLowercaseFieldsInOrderAndTheGivenLength) {
    HttpResponse res;
    res.setStatus(200, "OK");
    res.headers["X-Zeta"] = "z";
    res.headers["Content-Type"] = "text/plain";

    std::string out;
    ASSERT_TRUE(build_response_head(res, 5, out));
    EXPECT_EQ(out,
              "HTTP/1.1 200 OK\r\n"
              "content-type: text/plain\r\n"
              "x-zeta: z\r\n"
              "content-length: 5\r\n"
              "\r\n");
}

TEST(ResponseHeadWriter, IgnoresAnyContentLengthInTheMap) {
    HttpResponse res;
    res.headers["content-length"] = "999";
    res.headers["Content-Length"] = "1";

    std::string out;
    ASSERT_TRUE(build_response_head(res, 5, out));
    EXPECT_EQ(out.find("999"), std::string::npos) << out;
    EXPECT_NE(out.find("content-length: 5\r\n"), std::string::npos) << out;
    EXPECT_EQ(out.find("content-length: 1\r\n"), std::string::npos) << out;
}

TEST(ResponseHeadWriter, EmitsIdenticalSpellingsOnce) {
    HttpResponse res;
    res.headers["Cache-Control"] = "no-store";
    res.headers["cache-control"] = "no-store";

    std::string out;
    ASSERT_TRUE(build_response_head(res, 0, out));
    const auto first = out.find("cache-control:");
    ASSERT_NE(first, std::string::npos) << out;
    EXPECT_EQ(out.find("cache-control:", first + 1), std::string::npos) << out;
}

TEST(ResponseHeadWriter, RefusesSpellingsThatDisagree) {
    HttpResponse res;
    res.headers["Cache-Control"] = "no-store";
    res.headers["cache-control"] = "max-age=60";
    std::string out;
    EXPECT_FALSE(build_response_head(res, 0, out));
}

TEST(ResponseHeadWriter, RefusesInvalidFieldNames) {
    for (const char* name : {"", "X Note", "X-Note:", "X\r\nNote", " X-Note"}) {
        HttpResponse res;
        res.headers[name] = "v";
        std::string out;
        EXPECT_FALSE(build_response_head(res, 0, out)) << "name: '" << name << "'";
    }
}

TEST(ResponseHeadWriter, RefusesControlCharactersInValues) {
    const std::vector<std::string> values{
        "a\r\nb", "a\nb", "a\rb", std::string("a\0b", 3), "a\x7f"};
    for (const std::string& value : values) {
        HttpResponse res;
        res.headers["x-note"] = value;
        std::string out;
        EXPECT_FALSE(build_response_head(res, 0, out));
    }
}

TEST(ResponseHeadWriter, AcceptsTabsSpacesAndObsTextInValues) {
    HttpResponse res;
    res.headers["x-note"] = "a b\tc \xe2\x9c\x93";
    std::string out;
    EXPECT_TRUE(build_response_head(res, 0, out));
}

TEST(ResponseHeadWriter, RefusesTransferEncoding) {
    HttpResponse res;
    res.headers["transfer-encoding"] = "chunked";
    std::string out;
    EXPECT_FALSE(build_response_head(res, 0, out));
}

TEST(ResponseHeadWriter, RefusesOutOfRangeStatusCodes) {
    for (int code : {0, 42, 99, 600, 1000, -200}) {
        HttpResponse res;
        res.status_code = code;
        std::string out;
        EXPECT_FALSE(build_response_head(res, 0, out)) << code;
    }
    for (int code : {100, 204, 599}) {
        HttpResponse res;
        res.status_code = code;
        std::string out;
        EXPECT_TRUE(build_response_head(res, 0, out)) << code;
    }
}

TEST(ResponseHeadWriter, RefusesLineBreaksInTheReasonPhraseButAllowsItEmpty) {
    HttpResponse bad;
    bad.status_text = "OK\r\nx: y";
    std::string out;
    EXPECT_FALSE(build_response_head(bad, 0, out));

    HttpResponse empty;
    empty.status_text = "";
    ASSERT_TRUE(build_response_head(empty, 0, out));
    EXPECT_EQ(out.rfind("HTTP/1.1 200 \r\n", 0), 0u) << out;
}

TEST(ResponseHeadWriter, FallbackIsAValidCompleteResponse) {
    const std::string keep = fallback_error_response(false);
    const std::string close = fallback_error_response(true);
    for (const std::string& r : {keep, close}) {
        const auto end = r.find("\r\n\r\n");
        ASSERT_NE(end, std::string::npos);
        EXPECT_EQ(r.rfind("HTTP/1.1 500 ", 0), 0u);
        EXPECT_NE(r.find("content-length: 21\r\n"), std::string::npos) << r;
        EXPECT_EQ(r.size() - (end + 4), 21u) << r;
    }
    EXPECT_EQ(keep.find("connection:"), std::string::npos);
    EXPECT_NE(close.find("connection: close\r\n"), std::string::npos);
}

// setHeader() replaces a field whatever spelling put it in the map.
TEST(ResponseHeaderMap, SetHeaderReplacesOtherSpellings) {
    HttpResponse res;
    res.headers["Content-Type"] = "text/html";
    res.setHeader("content-type", "application/json");
    EXPECT_EQ(res.headers.size(), 1u);
    EXPECT_EQ(res.headers.at("content-type"), "application/json");
}

TEST(ResponseHeaderMap, NormalizeFoldsNamesAndReportsConflicts) {
    HttpResponse agree;
    agree.headers["X-A"] = "1";
    agree.headers["x-a"] = "1";
    agree.headers["Content-Type"] = "text/plain";
    EXPECT_TRUE(agree.normalizeHeaders());
    EXPECT_EQ(agree.headers.size(), 2u);
    EXPECT_EQ(agree.headers.at("content-type"), "text/plain");

    HttpResponse disagree;
    disagree.headers["X-A"] = "1";
    disagree.headers["x-a"] = "2";
    EXPECT_FALSE(disagree.normalizeHeaders());
}

}  // namespace
