#include "caduvelox/http/HttpRouter.hpp"
#include "caduvelox/http/UrlDecode.hpp"

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

void HttpRouter::dispatch(const HttpRequest& req, HttpResponse& res) const {
    // Match the path, not the whole request-target. req.path is the raw target,
    // query string included, and matching all of it meant an anchored route like
    // "^/api/items$" missed "/api/items?page=2" and a capture like "^/files/(.+)$"
    // swallowed the query into the filename. Handlers still get req.path whole.
    //
    // The common case has no query, so match req.path in place rather than copy it.
    const size_t query = req.path.find('?');
    std::string stripped;
    const std::string* route_path = &req.path;
    if (query != std::string::npos) {
        stripped.assign(req.path, 0, query);
        route_path = &stripped;
    }

    // Never hand std::regex an unbounded path. It matches recursively, one frame
    // per character: a long enough path overflows the ring thread's stack and
    // takes the process with it, and well before that it costs hundreds of
    // microseconds per route. See MAX_ROUTABLE_PATH. This check is the guard;
    // it does not rely on the parser's request-line limit, which was chosen for
    // other reasons and could be raised without anyone thinking of this.
    if (route_path->size() > MAX_ROUTABLE_PATH) {
        handle_uri_too_long(res);
        return;
    }

    // Percent-decode before matching, so a route and a capture see the path the
    // client meant: "/files/a%20b.txt" is a request for the file "a b.txt", and
    // without this it could not be served at all. Segments are decoded
    // individually -- see UrlDecode.hpp for why, and for what is refused.
    //
    // Skipped entirely when there is no escape to expand, which is the usual
    // case: then the raw path already is the decoded path.
    std::string decoded;
    if (route_path->find('%') != std::string::npos) {
        if (!url::decodePath(*route_path, decoded)) {
            handle_bad_request(res);
            return;
        }
        route_path = &decoded;
    }

    for (const auto& route : routes_) {
        if (route.method != "ALL" && route.method != req.method) continue;
        
        // match_results points into *route_path, which outlives the handler call.
        std::smatch match_results;
        if (std::regex_match(*route_path, match_results, route.path_regex)) {
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

            // Handlers may write the header map directly, in any case. Fold the
            // names *before* the defaults below look for them: otherwise a
            // handler's "Content-Type" is missed and a second, default one is
            // added beside it, and both reach the wire.
            if (!res.normalizeHeaders()) {
                // Two spellings of one field with different values -- there is no
                // honest way to pick, so the handler's response is not sent.
                res = HttpResponse{};
                res.setStatus(500, "Internal Server Error");
                res.setBody("Internal Server Error");
            }
            
            fallback_to_default_headers(res);
            return;
        }
    }
    handle_not_found(res);
}

void HttpRouter::fallback_to_default_headers(HttpResponse& res) const {
    if (res.getHeader("content-length").empty()) {
        res.setHeader("content-length", std::to_string(res.body.size()));
    }
    
    // Only set default content-type for body responses, not file responses
    // File responses will have their content-type set by HTTPFileJob based on file extension
    if (res.getHeader("content-type").empty() && res.file_path.empty()) {
        res.setHeader("content-type", "text/plain");
    }
}

void HttpRouter::handle_bad_request(HttpResponse& res) const {
    // The path could not be decoded: a broken escape, or one that would have
    // manufactured a separator or a parent-directory step.
    res.setStatus(400, "Bad Request");
    res.setBody("Bad Request");
    res.setHeader("content-type", "text/plain");
}

void HttpRouter::handle_uri_too_long(HttpResponse& res) const {
    // RFC 9110 section 15.5.15.
    res.setStatus(414, "URI Too Long");
    res.setBody("URI Too Long");
    res.setHeader("content-type", "text/plain");
}

void HttpRouter::handle_not_found(HttpResponse& res) const {
    res.setStatus(404, "Not Found");
    res.setBody("Not Found");
    res.setHeader("content-type", "text/plain");
    res.setHeader("content-length", std::to_string(res.body.size()));
}

} // namespace caduvelox
