#include "caduvelox/http/HttpParser.hpp"
#include <cctype>

namespace caduvelox {

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
    
    // Handle body based on Content-Length
    size_t content_length = 0;
    if (auto it = out.headers.find("content-length"); it != out.headers.end()) {
        content_length = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
        
        // Reject unreasonably large content-length
        if (content_length > 1024 * 1024 * 1024) { // 1GB limit
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

    std::string_view version = line.substr(sp2 + 1);
    if (version.empty()) return false;

    out.method.assign(line.data(), sp1);
    out.path.assign(line.data() + sp1 + 1, sp2 - sp1 - 1);
    out.version.assign(version.data(), version.size());
    return true;
}

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
        if (colon != std::string::npos) {
            std::string name = normalize_header_name(line.substr(0, colon));
            std::string value = trim_header_value(line.substr(colon + 1));
            
            // Reject Transfer-Encoding: chunked (not supported by minimal parser)
            if (name == "transfer-encoding") {
                std::string lv = value;
                std::transform(lv.begin(), lv.end(), lv.begin(), 
                    [](unsigned char c){ return std::tolower(c); });
                if (lv.find("chunked") != std::string::npos) {
                    return false;
                }
            }
            
            headers[std::move(name)] = std::move(value);
        }
        
        pos = eol + 2;
    }
    
    out.headers = std::move(headers);
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
