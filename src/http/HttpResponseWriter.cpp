#include "caduvelox/http/HttpResponseWriter.hpp"
#include "caduvelox/http/HttpParser.hpp"
#include <cctype>
#include <map>
#include <string_view>

namespace caduvelox {
namespace {

// RFC 9112 section 4: reason-phrase = 1*( HTAB / SP / VCHAR / obs-text ).
// Empty is tolerated -- "HTTP/1.1 200 " is a valid status line.
bool is_valid_reason_phrase(std::string_view text) {
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc == '\t') continue;
        if (uc < 0x20 || uc == 0x7F) return false;
    }
    return true;
}

std::string lowercase(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

bool build_response_head(const HttpResponse& res, uint64_t content_length, std::string& out) {
    if (res.status_code < 100 || res.status_code > 599) return false;
    if (!is_valid_reason_phrase(res.status_text)) return false;

    // Ordered so the wire output is deterministic, which also makes it testable.
    std::map<std::string, const std::string*> fields;
    for (const auto& [name, value] : res.headers) {
        if (!HttpParser::is_valid_field_name(name)) return false;
        if (!HttpParser::is_valid_field_value(value)) return false;

        std::string key = lowercase(name);
        if (key == "transfer-encoding") return false;
        if (key == "content-length") continue;  // written below, from the real length

        auto [it, inserted] = fields.emplace(std::move(key), &value);
        if (!inserted && *it->second != value) {
            // "Content-Type: a" and "content-type: b": no honest way to pick.
            return false;
        }
    }

    out.clear();
    out += "HTTP/1.1 ";
    out += std::to_string(res.status_code);
    out += ' ';
    out += res.status_text;
    out += "\r\n";
    for (const auto& [name, value] : fields) {
        out += name;
        out += ": ";
        out += *value;
        out += "\r\n";
    }
    out += "content-length: ";
    out += std::to_string(content_length);
    out += "\r\n\r\n";
    return true;
}

std::string fallback_error_response(bool close_connection) {
    static constexpr std::string_view kBody = "Internal Server Error";
    std::string out = "HTTP/1.1 500 Internal Server Error\r\n"
                      "content-type: text/plain\r\n"
                      "content-length: " + std::to_string(kBody.size()) + "\r\n";
    if (close_connection) {
        out += "connection: close\r\n";
    }
    out += "\r\n";
    out += kBody;
    return out;
}

} // namespace caduvelox
