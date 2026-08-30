#pragma once
#include "caduvelox/http/HttpTypes.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
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

    // Largest body this parser will agree to buffer. A request declaring more
    // is rejected outright rather than accumulated.
    static constexpr size_t MAX_CONTENT_LENGTH = 1024ull * 1024 * 1024;  // 1 GiB

    // Parse a single HTTP/1.1 request from buffer
    // Returns ParseResult indicating success, need-more-data, or fatal error
    // On Success: consumed is set to bytes used, out contains parsed request
    // On Incomplete: consumed is 0, caller should wait for more data
    // On BadRequest: consumed is 0, caller should close connection or send 400
    //
    // This parser is deliberately strict about anything that a proxy in front of
    // it could read differently -- framing headers, field syntax, and embedded
    // control characters. See the note above parse_headers() in the .cpp.
    static ParseResult parse_request(std::string_view buf, HttpRequest& out, size_t& consumed);

  private:
    static bool parse_request_line(std::string_view line, HttpRequest& out);
    static bool parse_headers(std::string_view headers_section, HttpRequest& out);
    static std::string trim_header_value(std::string_view value);
    static std::string normalize_header_name(std::string_view name);

    // Content-Length = 1*DIGIT (RFC 9110 section 8.6). Returns false for anything
    // else, including an empty value, a sign, trailing junk, or a value over
    // MAX_CONTENT_LENGTH.
    static bool parse_content_length(std::string_view value, size_t& out);

    // RFC 9110 field-name = token. Rejects the empty name, leading whitespace
    // (obs-fold continuation lines), and whitespace before the colon.
    static bool is_valid_field_name(std::string_view name);

    // RFC 9110 field-value: visible characters, obs-text, plus interior SP/HTAB.
    // The point is to reject embedded CR/LF and other control characters.
    static bool is_valid_field_value(std::string_view value);
};

} // namespace caduvelox
