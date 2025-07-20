#include "caduvelox/http/HttpRouter.hpp"

namespace caduvelox {

HttpRouter::HttpRouter(std::vector<Route> routes) : routes_(std::move(routes)) {}

HttpRouter::HttpRouter(const HttpRouter& other) {
    routes_.reserve(other.routes_.size());
    for (const auto& route : other.routes_) {
        routes_.push_back(Route{
            route.method, 
            route.path_regex, 
            route.handler,
            route.handler_with_captures,  // Copy capture-based handler
            route.uses_captures           // Copy flag
        });
    }
}

HttpRouter& HttpRouter::operator=(const HttpRouter& other) {
    if (this != &other) {
        routes_.clear();
        routes_.reserve(other.routes_.size());
        for (const auto& route : other.routes_) {
            routes_.push_back(Route{
                route.method, 
                route.path_regex, 
                route.handler,
                route.handler_with_captures,  // Copy capture-based handler
                route.uses_captures           // Copy flag
            });
        }
    }
    return *this;
}

void HttpRouter::get(const std::string& pathRegex, HttpHandler handler) {
    addRoute("GET", pathRegex, std::move(handler));
}

void HttpRouter::post(const std::string& pathRegex, HttpHandler handler) {
    addRoute("POST", pathRegex, std::move(handler));
}

void HttpRouter::put(const std::string& pathRegex, HttpHandler handler) {
    addRoute("PUT", pathRegex, std::move(handler));
}

void HttpRouter::del(const std::string& pathRegex, HttpHandler handler) {
    addRoute("DELETE", pathRegex, std::move(handler));
}

void HttpRouter::all(const std::string& pathRegex, HttpHandler handler) {
    addRoute("ALL", pathRegex, std::move(handler));
}

void HttpRouter::addRoute(const std::string& method, const std::string& pathRegex, HttpHandler handler) {
    routes_.push_back(Route{method, std::regex(pathRegex), std::move(handler), nullptr, false});
}

// Capture-aware route methods
void HttpRouter::getWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler) {
    addRouteWithCaptures("GET", pathRegex, std::move(handler));
}

void HttpRouter::postWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler) {
    addRouteWithCaptures("POST", pathRegex, std::move(handler));
}

void HttpRouter::putWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler) {
    addRouteWithCaptures("PUT", pathRegex, std::move(handler));
}

void HttpRouter::delWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler) {
    addRouteWithCaptures("DELETE", pathRegex, std::move(handler));
}

void HttpRouter::allWithCaptures(const std::string& pathRegex, HttpHandlerWithCaptures handler) {
    addRouteWithCaptures("ALL", pathRegex, std::move(handler));
}

void HttpRouter::addRouteWithCaptures(const std::string& method, const std::string& pathRegex, HttpHandlerWithCaptures handler) {
    routes_.push_back(Route{method, std::regex(pathRegex), nullptr, std::move(handler), true});
}

void HttpRouter::dispatch(const HttpRequest& req, HttpResponse& res) {
    for (const auto& route : routes_) {
        if (route.method != "ALL" && route.method != req.method) continue;
        
        std::smatch match_results;
        if (std::regex_match(req.path, match_results, route.path_regex)) {
            try {
                if (route.uses_captures && route.handler_with_captures) {
                    // Use capture-aware handler with match results
                    route.handler_with_captures(req, res, match_results);
                } else if (!route.uses_captures && route.handler) {
                    // Use basic handler (no captures needed)
                    route.handler(req, res);
                } else {
                    // Configuration error - route setup incorrectly
                    res.setStatus(500, "Internal Server Error");
                    res.setBody("Route configuration error");
                }
            }
            catch (...) { 
                res.setStatus(500, "Internal Server Error"); 
                res.setBody("Internal Server Error"); 
            }
            
            fallback_to_default_headers(res);
            return;
        }
    }
    handle_not_found(res);
}

void HttpRouter::fallback_to_default_headers(HttpResponse& res) {
    if (res.getHeader("content-length").empty()) {
        res.setHeader("content-length", std::to_string(res.body.size()));
    }
    
    // Only set default content-type for body responses, not file responses
    // File responses will have their content-type set by HTTPFileJob based on file extension
    if (res.getHeader("content-type").empty() && res.file_path.empty()) {
        res.setHeader("content-type", "text/plain");
    }
}

void HttpRouter::handle_not_found(HttpResponse& res) {
    res.setStatus(404, "Not Found");
    res.setBody("Not Found");
    res.setHeader("content-type", "text/plain");
    res.setHeader("content-length", std::to_string(res.body.size()));
}

} // namespace caduvelox
