#include <gtest/gtest.h>
#include "caduvelox/http/HttpRouter.hpp"
#include <cstdlib>
#include <string>

using namespace caduvelox;

/**
 * What the router matches against, and how much of it (review item M2).
 *
 * Routes are std::regex patterns applied to the request-target. Two problems,
 * both measured before changing anything:
 *
 *  - libstdc++'s regex matcher recurses once per input character. Against
 *    "^/files/.*$" it cost 79 us for a 1 KiB path, 540 us for 8 KiB -- about 30x
 *    a whole normal request -- and it overflowed an 8 MB stack (SIGSEGV, the whole
 *    process) somewhere between 32 KiB and 64 KiB. Ring threads use the default
 *    8 MB stack. The only thing standing between a client and that crash was the
 *    parser's 8 KiB request-line limit, which was chosen for parsing, not for this.
 *
 *  - The request-target includes the query string, and the router matched against
 *    all of it. So an anchored route like "^/api/items$" did not match
 *    "/api/items?page=2", and a capture like "^/files/(.+)$" captured the query
 *    along with the filename.
 *
 * The router now matches the path only -- the target up to the first '?' -- and
 * refuses to run a regex over a path longer than HttpRouter::MAX_ROUTABLE_PATH,
 * answering 414 URI Too Long instead. req.path itself is untouched, so handlers
 * still see the query.
 */
namespace {

HttpRequest makeRequest(const std::string& method, const std::string& target) {
    HttpRequest req;
    req.method = method;
    req.path = target;
    req.version = "HTTP/1.1";
    return req;
}

// ---------------------------------------------------------------------------
// The query string is not part of the route
// ---------------------------------------------------------------------------

TEST(HttpRouterQuery, AnAnchoredRouteMatchesWhenAQueryIsPresent) {
    HttpRouter router;
    bool hit = false;
    router.get("^/api/items$", [&](const HttpRequest&, HttpResponse& res) {
        hit = true;
        res.setBody("items");
    });

    HttpResponse res;
    router.dispatch(makeRequest("GET", "/api/items?page=2"), res);

    EXPECT_TRUE(hit)
        << "the route did not match because the query string was treated as part "
           "of the path";
    EXPECT_EQ(res.status_code, 200);
}

TEST(HttpRouterQuery, CapturesDoNotIncludeTheQuery) {
    HttpRouter router;
    std::string captured;
    router.getWithCaptures("^/files/(.+)$",
        [&](const HttpRequest&, HttpResponse& res, const std::smatch& m) {
            captured = m[1].str();
            res.setBody("ok");
        });

    HttpResponse res;
    router.dispatch(makeRequest("GET", "/files/index.html?v=3"), res);

    EXPECT_EQ(captured, "index.html")
        << "a file route would look for a file literally named 'index.html?v=3'";
}

// Guard: the handler still sees the full request-target, query included.
TEST(HttpRouterQuery, HandlersStillSeeTheFullTarget) {
    HttpRouter router;
    std::string seen;
    router.get("^/search$", [&](const HttpRequest& req, HttpResponse& res) {
        seen = req.path;
        res.setBody("ok");
    });

    HttpResponse res;
    router.dispatch(makeRequest("GET", "/search?q=io_uring&page=2"), res);

    EXPECT_EQ(seen, "/search?q=io_uring&page=2")
        << "only matching ignores the query; the request itself is unchanged";
}

// Guard: a path with no query routes exactly as before.
TEST(HttpRouterQuery, PathsWithoutAQueryRouteAsBefore) {
    HttpRouter router;
    std::string captured;
    router.getWithCaptures(R"(^/users/(\d+)$)",
        [&](const HttpRequest&, HttpResponse& res, const std::smatch& m) {
            captured = m[1].str();
            res.setBody("ok");
        });

    HttpResponse res;
    router.dispatch(makeRequest("GET", "/users/42"), res);
    EXPECT_EQ(captured, "42");

    HttpResponse miss;
    router.dispatch(makeRequest("GET", "/users/abc"), miss);
    EXPECT_EQ(miss.status_code, 404);
}

// ---------------------------------------------------------------------------
// Length: the regex never sees an unbounded path
// ---------------------------------------------------------------------------

TEST(HttpRouterLength, AnOverlongPathIsRefusedWith414WithoutRunningAHandler) {
    HttpRouter router;
    bool hit = false;
    router.get("^/files/.*$", [&](const HttpRequest&, HttpResponse& res) {
        hit = true;
        res.setBody("ok");
    });

    // Longer than the bound, and well within what the parser accepts (8 KiB).
    const std::string target = "/files/" + std::string(6000, 'a');
    HttpResponse res;
    router.dispatch(makeRequest("GET", target), res);

    EXPECT_EQ(res.status_code, 414)
        << "a path this long used to be matched in full: ~400 us of ring-thread "
           "CPU per route that contains '.*'";
    EXPECT_FALSE(hit);
}

// Guard: the query does not count against the bound, only the path does.
TEST(HttpRouterLength, ALongQueryDoesNotCountAgainstTheBound) {
    HttpRouter router;
    bool hit = false;
    router.get("^/search$", [&](const HttpRequest&, HttpResponse& res) {
        hit = true;
        res.setBody("ok");
    });

    HttpResponse res;
    router.dispatch(makeRequest("GET", "/search?q=" + std::string(6000, 'x')), res);

    EXPECT_TRUE(hit) << "long query strings are ordinary; only the path is bounded";
    EXPECT_EQ(res.status_code, 200);
}

// Guard: a path exactly at the bound still routes.
TEST(HttpRouterLength, APathAtTheBoundStillRoutes) {
    HttpRouter router;
    bool hit = false;
    router.get("^/files/.*$", [&](const HttpRequest&, HttpResponse& res) {
        hit = true;
        res.setBody("ok");
    });

    const std::string prefix = "/files/";
    const std::string target = prefix + std::string(HttpRouter::MAX_ROUTABLE_PATH - prefix.size(), 'a');
    ASSERT_EQ(target.size(), HttpRouter::MAX_ROUTABLE_PATH);

    HttpResponse res;
    router.dispatch(makeRequest("GET", target), res);
    EXPECT_TRUE(hit);
    EXPECT_EQ(res.status_code, 200);
}

/**
 * The crash itself. Run in a forked child: before the fix, a 64 KiB path
 * overflows the stack inside std::regex_match and the child dies with SIGSEGV,
 * which fails this test instead of taking the whole test binary down with it.
 *
 * The parser's 8 KiB limit means a client cannot deliver this through the server
 * today. That is exactly the point: the router's safety should not depend on a
 * limit set for an unrelated reason.
 */
TEST(HttpRouterLengthDeathTest, APathLongEnoughToOverflowTheRegexStackDoesNotCrash) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        HttpRouter router;
        router.get("^/files/.*$", [](const HttpRequest&, HttpResponse& res) {
            res.setBody("ok");
        });
        HttpResponse res;
        router.dispatch(makeRequest("GET", "/files/" + std::string(64 * 1024, 'a')), res);
        std::exit(res.status_code == 414 ? 0 : 1);
    }, ::testing::ExitedWithCode(0), "");
}

}  // namespace
