#include <gtest/gtest.h>
#include "caduvelox/http/UrlDecode.hpp"
#include <string>

using namespace caduvelox;

/**
 * url::decodePath() on its own (review item M12). The end-to-end behaviour is in
 * path_decoding_test; these pin the rules one at a time.
 */
namespace {

// Decodes to `expected`.
void expectDecodes(const std::string& raw, const std::string& expected) {
    std::string out;
    ASSERT_TRUE(url::decodePath(raw, out)) << "refused: " << raw;
    EXPECT_EQ(out, expected) << "input: " << raw;
}

void expectRefused(const std::string& raw) {
    std::string out;
    EXPECT_FALSE(url::decodePath(raw, out)) << "accepted: " << raw << " -> " << out;
}

TEST(UrlDecodePath, LeavesAnUnescapedPathAlone) {
    expectDecodes("/files/plain.txt", "/files/plain.txt");
    expectDecodes("/", "/");
    expectDecodes("", "");
    expectDecodes("/a//b/", "/a//b/");
}

TEST(UrlDecodePath, DecodesOrdinaryEscapes) {
    expectDecodes("/files/a%20b.txt", "/files/a b.txt");
    expectDecodes("/files/caf%C3%A9.txt", "/files/caf\xc3\xa9.txt");
    expectDecodes("/%61%62%63", "/abc");
    expectDecodes("/a%2Bb", "/a+b");
}

TEST(UrlDecodePath, AcceptsBothCasesOfHex) {
    expectDecodes("/%2d%2D", "/--");
}

TEST(UrlDecodePath, LeavesPlusAlone) {
    // '+' is a space in a query string, never in a path (RFC 3986).
    expectDecodes("/files/a+b.txt", "/files/a+b.txt");
}

TEST(UrlDecodePath, RefusesMalformedEscapes) {
    expectRefused("/files/a%");
    expectRefused("/files/a%2");
    expectRefused("/files/a%zz");
    expectRefused("/files/a%2z");
    expectRefused("/%");
}

TEST(UrlDecodePath, RefusesControlCharacters) {
    expectRefused("/files/a%00b");      // NUL would truncate the name downstream
    expectRefused("/files/a%0ab");      // LF
    expectRefused("/files/a%0db");      // CR
    expectRefused("/files/a%09b");      // HTAB
    expectRefused("/files/a%7fb");      // DEL
}

TEST(UrlDecodePath, RefusesAnEncodedSeparator) {
    // Decoding this into '/' would create a segment boundary that no ".." check
    // upstream could have seen.
    expectRefused("/files/sub%2fplain.txt");
    expectRefused("/files/sub%2Fplain.txt");
}

TEST(UrlDecodePath, RefusesEscapesThatSpellDotDot) {
    expectRefused("/files/%2e%2e/secret");
    expectRefused("/files/%2E%2E/secret");
    expectRefused("/files/.%2e/secret");
    expectRefused("/files/%2e./secret");
}

// A literal ".." is the caller's business, and always was: it reaches the
// handler, where confining a path to a document root is decided.
TEST(UrlDecodePath, PassesALiteralDotDotThrough) {
    expectDecodes("/files/../index.html", "/files/../index.html");
    expectDecodes("/files/.", "/files/.");
}

TEST(UrlDecodePath, DecodesOnlyOnce) {
    // %252e is '%' '2' 'e', not '.'. Decoding twice would reach "..".
    expectDecodes("/files/%252e%252e/x", "/files/%2e%2e/x");
}

TEST(UrlDecodePath, DecodesEachSegmentIndependently) {
    expectDecodes("/a%20b/c%20d/e", "/a b/c d/e");
    // A refusal anywhere refuses the whole path.
    expectRefused("/a%20b/%2e%2e/e");
}

}  // namespace
