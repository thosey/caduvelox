#pragma once
#include "caduvelox/http/HttpTypes.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <sstream>
#include <algorithm>

namespace caduvelox {

class HttpParser {
  public:
    // Parse result enum to distinguish between incomplete, success, and fatal errors
    enum class ParseResult {
        Success,        // Complete request parsed successfully
        Incomplete,     // Need more data (not an error)
        BadRequest      // Fatal parse error (malformed request, unsupported features, etc.)
    };

    // Basic parser limits to avoid pathological requests
    static constexpr size_t MAX_REQUEST_LINE = 8 * 1024;      // 8 KiB
    static constexpr size_t MAX_HEADERS = 200;               // max header count
    static constexpr size_t MAX_HEADER_LINE = 16 * 1024;     // 16 KiB per header

    // Parse a single HTTP/1.1 request from buffer
    // Returns ParseResult indicating success, need-more-data, or fatal error
    // On Success: consumed is set to bytes used, out contains parsed request
    // On Incomplete: consumed is 0, caller should wait for more data
    // On BadRequest: consumed is 0, caller should close connection or send 400
    static ParseResult parse_request(std::string_view buf, HttpRequest& out, size_t& consumed);

  private:
    static bool parse_request_line(std::string_view line, HttpRequest& out);
    static bool parse_headers(std::string_view headers_section, HttpRequest& out);
    static std::string trim_header_value(std::string_view value);
    static std::string normalize_header_name(std::string_view name);
};

} // namespace caduvelox
