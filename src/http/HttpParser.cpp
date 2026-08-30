#include "caduvelox/http/HttpParser.hpp"
#include <cctype>

namespace caduvelox {
namespace {

// RFC 9110 tchar: the characters allowed in a token (method names, field names).
bool is_tchar(unsigned char c) {
    // Spelled out rather than via std::isalnum, which is locale-dependent and
    // can classify bytes >= 0x80 as alphanumeric.
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

// Every character a request-target or HTTP-version may contain: VCHAR only.
// No spaces (the request line is already split on them), no HTAB, no controls.
bool is_vchar(unsigned char c) { return c > 0x20 && c != 0x7F; }

}  // namespace

HttpParser::ParseResult HttpParser::parse_request(std::string_view buf, HttpRequest& out, size_t& consumed) {
    consumed = 0;
    if (buf.empty()) return ParseResult::Incomplete;
    
    // Find end of headers
    size_t header_end = buf.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        // Incomplete headers - but check if we've exceeded limits with partial data
        if (buf.size() > MAX_REQUEST_LINE + (MAX_HEADERS * MAX_HEADER_LINE)) {
            // Request is already too large without finding end of headers
            return ParseResult::BadRequest;
        }
        return ParseResult::Incomplete;
    }
    
    size_t headers_bytes = header_end + 4;
    std::string_view headers_section(buf.data(), headers_bytes);
    
    // Parse request line
    size_t line_end = headers_section.find("\r\n");
    if (line_end == std::string::npos) return ParseResult::BadRequest; // Malformed
    
    // Enforce request-line length limit
    if (line_end > MAX_REQUEST_LINE) return ParseResult::BadRequest;
    
    std::string_view reqline = headers_section.substr(0, line_end);
    if (!parse_request_line(reqline, out)) return ParseResult::BadRequest;
    
    // Parse headers
    std::string_view headers_only = headers_section.substr(line_end + 2, headers_bytes - line_end - 4);
    if (!parse_headers(headers_only, out)) return ParseResult::BadRequest;
    
    // Handle body based on Content-Length.
    //
    // A value this parser cannot make exact sense of is a fatal error, never a
    // silent zero. Framing is the one thing a front-end proxy and this server
    // must agree on byte for byte: if they disagree about where this request
    // ends, the tail of it becomes the head of the *next* request as far as one
    // of them is concerned, and an attacker chooses what that next request says.
    size_t content_length = 0;
    if (auto it = out.headers.find("content-length"); it != out.headers.end()) {
        if (!parse_content_length(it->second, content_length)) {
            return ParseResult::BadRequest;
        }
    }
    
    size_t total_needed = headers_bytes + content_length;
    if (buf.size() < total_needed) return ParseResult::Incomplete;
    
    if (content_length) {
        out.body.assign(buf.data() + headers_bytes, content_length);
    } else {
        out.body.clear();
    }
    
    consumed = total_needed;
    return ParseResult::Success;
}

bool HttpParser::parse_request_line(std::string_view line, HttpRequest& out) {
    size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos || sp1 == 0) return false;

    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || sp2 == sp1 + 1) return false;

    std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = line.substr(sp2 + 1);
    if (version.empty()) return false;

    // The method is a token; the target and version are VCHAR runs. Enforcing
    // this rejects a request line carrying an embedded HTAB, NUL, or bare LF --
    // the last of which a recipient that splits on LF rather than CRLF would
    // read as an extra header line.
    if (!is_valid_field_name(method)) return false;
    for (char c : target) {
        if (!is_vchar(static_cast<unsigned char>(c))) return false;
    }
    for (char c : version) {
        if (!is_vchar(static_cast<unsigned char>(c))) return false;
    }

    // HTTP-version = "HTTP/" DIGIT "." DIGIT. Checked in full rather than by
    // substring, because shouldKeepAlive() decides persistence with
    // version.find("1.1") and would otherwise honour "HTTP/9.1.1".
    if (version.size() != 8 || version.substr(0, 5) != "HTTP/" ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9') {
        return false;
    }

    out.method.assign(method.data(), method.size());
    out.path.assign(target.data(), target.size());
    out.version.assign(version.data(), version.size());
    return true;
}

// Header parsing is strict on purpose. Anything accepted here that a proxy in
// front of this server would read differently is a request-desync primitive:
// obs-fold continuation lines, whitespace between the field name and the colon,
// a line with no colon at all, control characters inside a value, and
// contradictory Content-Length headers are all ways to make two recipients
// disagree about where one request stops and the next starts.
bool HttpParser::parse_headers(std::string_view headers_section, HttpRequest& out) {
    std::unordered_map<std::string, std::string> headers;
    size_t pos = 0;
    size_t header_count = 0;
    
    while (pos < headers_section.size()) {
        size_t eol = headers_section.find("\r\n", pos);
        if (eol == std::string::npos) break;
        
        std::string_view line = headers_section.substr(pos, eol - pos);
        if (line.empty()) break;
        
        // Enforce per-header line length
        if (line.size() > MAX_HEADER_LINE) return false;
        
        ++header_count;
        if (header_count > MAX_HEADERS) return false;
        
        size_t colon = line.find(':');
        if (colon == std::string_view::npos) return false;

        std::string_view raw_name = line.substr(0, colon);
        std::string_view raw_value = line.substr(colon + 1);
        if (!is_valid_field_name(raw_name)) return false;
        if (!is_valid_field_value(raw_value)) return false;

        std::string name = normalize_header_name(raw_name);
        std::string value = trim_header_value(raw_value);

        // Reject Transfer-Encoding: chunked (not supported by minimal parser)
        if (name == "transfer-encoding") {
            std::string lv = value;
            std::transform(lv.begin(), lv.end(), lv.begin(),
                [](unsigned char c){ return std::tolower(c); });
            if (lv.find("chunked") != std::string::npos) {
                return false;
            }
        }

        // try_emplace leaves both arguments untouched when it does not insert,
        // so `value` is still readable on the duplicate path below.
        auto [slot, inserted] = headers.try_emplace(std::move(name), std::move(value));
        if (!inserted) {
            // RFC 9110 section 8.6: repeated Content-Length is tolerable only when
            // every value is identical. Differing values make the message
            // invalid, and picking one of them is precisely how a CL.CL desync
            // gets its two answers.
            if (slot->first == "content-length" && slot->second != value) {
                return false;
            }
            // Any other repeated field keeps the long-standing last-wins
            // behaviour. It is lossy for list-valued fields, but it cannot move
            // a message boundary.
            slot->second = std::move(value);
        }

        pos = eol + 2;
    }
    
    out.headers = std::move(headers);
    return true;
}

bool HttpParser::parse_content_length(std::string_view value, size_t& out) {
    // Digits and nothing else. std::strtoul, which this replaced, returned 0 for
    // "abc" -- so a non-numeric Content-Length used to leave the body bytes in
    // the buffer to be parsed as the following request. It also silently
    // accepted leading whitespace, "+", and "-" (wrapping negatives around).
    if (value.empty()) return false;

    size_t result = 0;
    for (char c : value) {
        if (c < '0' || c > '9') return false;
        const size_t digit = static_cast<size_t>(c - '0');
        // Compare against the cap before multiplying rather than after, so an
        // absurdly long digit string is rejected on its own terms instead of
        // wrapping into an acceptable-looking value. This subsumes the old
        // post-hoc "> 1 GiB" check.
        if (result > (MAX_CONTENT_LENGTH - digit) / 10) return false;
        result = result * 10 + digit;
    }

    out = result;
    return true;
}

bool HttpParser::is_valid_field_name(std::string_view name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (!is_tchar(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool HttpParser::is_valid_field_value(std::string_view value) {
    // SP and HTAB are fine anywhere here; trim_header_value() strips the ones at
    // the edges afterwards. Everything below 0x20 other than HTAB, plus DEL, is
    // rejected -- most importantly a bare CR or LF, which a recipient that
    // splits lines on LF alone would treat as the start of another header.
    for (char c : value) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc == '\t') continue;
        if (uc < 0x20 || uc == 0x7F) return false;
    }
    return true;
}

std::string HttpParser::trim_header_value(std::string_view value) {
    size_t start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    size_t end = value.find_last_not_of(" \t");
    return std::string(value.substr(start, end - start + 1));
}

std::string HttpParser::normalize_header_name(std::string_view name) {
    std::string result(name);
    std::transform(result.begin(), result.end(), result.begin(), 
        [](unsigned char c){ return std::tolower(c); });
    return result;
}

} // namespace caduvelox
