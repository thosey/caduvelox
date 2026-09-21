#pragma once
#include "caduvelox/http/HttpTypes.hpp"
#include <vector>
#include <regex>
#include <string>

namespace caduvelox {

class HttpRouter {
  public:
    /**
     * Longest path, in bytes, the router will run a route regex over. Anything
     * longer is answered 414 URI Too Long without matching.
     *
     * libstdc++'s std::regex matches recursively, one frame per input character.
     * Measured: "^/files/.*$" costs ~68 ns per character (540 us at 8 KiB), and
     * overflows an 8 MB stack -- the default for ring threads -- somewhere between
     * 32 KiB and 64 KiB, taking the whole process down. At 2 KiB the worst case
     * measured ~165 us for a request that reaches a "^/files/.*$" route -- still
     * about 10x a normal request, but bounded -- and the crash is at least 16x
     * away. Far longer than any path a real client sends.
     *
     * Only the path counts: the query string is not matched (see dispatch()).
     *
     * What this does NOT protect against: a route pattern with nested quantifiers,
     * such as "^(a+)+$" or "^(\w+\s?)*$", backtracks exponentially and can take
     * seconds on input far shorter than this limit. Patterns are supplied by the
     * application, so keep them anchored and free of nested repetition.
     */
    static constexpr size_t MAX_ROUTABLE_PATH = 2048;

    struct Route {
        std::string method;                          // e.g. "GET", "POST", or "ALL"
        std::regex path_regex;                       // e.g. R"(^/items/\d+$)"
        HttpHandler handler;                         // Basic handler (no captures)
        HttpHandlerWithCaptures handler_with_captures; // Enhanced handler (with captures)
        bool uses_captures;                          // True if handler_with_captures should be used
    };

    explicit HttpRouter(std::vector<Route> routes = {});
    
    // Copy constructor and assignment operator for factory usage
    HttpRouter(const HttpRouter& other);
    HttpRouter& operator=(const HttpRouter& other);
    
    // Move constructor and assignment operator
    HttpRouter(HttpRouter&& other) noexcept = default;
    HttpRouter& operator=(HttpRouter&& other) noexcept = default;

    // Add routes (basic handlers)
    void get(const std::string& pathRegex, HttpHandler handler);
    void post(const std::string& pathRegex, HttpHandler handler);
    void put(const std::string& pathRegex, HttpHandler handler);
    void del(const std::string& pathRegex, HttpHandler handler);
    void all(const std::string& pathRegex, HttpHandler handler);
    void addRoute(const std::string& method, const std::string& pathRegex, HttpHandler handler);

    // Add routes with capture group support (optimized for routes with regex captures)
    void getWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler);
    void postWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler);
    void putWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler);
    void delWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler);
    void allWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler);
    void addRouteWithCaptures(const std::string& method, const std::string& pathRegex, HttpHandlerWithCaptures handler);

    // Route a request and generate response.
    //
    // Routes match the path only -- the request-target up to the first '?' -- so
    // a query string never stops a route matching and never leaks into a
    // capture. req.path is passed to the handler unchanged. A path longer than
    // MAX_ROUTABLE_PATH is answered 414 without running any regex.
    void dispatch(const HttpRequest& req, HttpResponse& res) const;

  private:
    std::vector<Route> routes_;
    
    void fallback_to_default_headers(HttpResponse& res) const;
    void handle_not_found(HttpResponse& res) const;
    void handle_uri_too_long(HttpResponse& res) const;
};

} // namespace caduvelox
