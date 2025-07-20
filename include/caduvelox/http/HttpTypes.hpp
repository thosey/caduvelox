#pragma once
#include "caduvelox/http/HttpRequest.hpp"
#include "caduvelox/http/HttpResponse.hpp"
#include <functional>
#include <regex>

namespace caduvelox {

// Handler function types for HTTP route callbacks

// Basic handler - receives request and response
using HttpHandler = std::function<void(const HttpRequest &req, HttpResponse &res)>;

// Enhanced handler that receives regex match results (for routes with capture groups)
// If the route has capture groups, match[0] is the full match, match[1] is first capture group, etc.
using HttpHandlerWithCaptures = std::function<void(const HttpRequest &req, HttpResponse &res, const std::smatch &match)>;

} // namespace caduvelox