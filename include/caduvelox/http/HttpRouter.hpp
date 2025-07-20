#pragma once
#include "caduvelox/http/HttpTypes.hpp"
#include <vector>
#include <regex>
#include <string>

namespace caduvelox {

class HttpRouter {
  public:
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

    // Route a request and generate response
    void dispatch(const HttpRequest& req, HttpResponse& res);

  private:
    std::vector<Route> routes_;
    
    void fallback_to_default_headers(HttpResponse& res);
    void handle_not_found(HttpResponse& res);
};

} // namespace caduvelox
