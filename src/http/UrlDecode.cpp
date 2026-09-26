#include "caduvelox/http/UrlDecode.hpp"

namespace caduvelox::url {
namespace {

// Value of one hex digit, or -1. Spelled out rather than via std::isxdigit,
// which is locale-dependent.
int hexValue(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool decodePath(std::string_view raw_path, std::string& out) {
    out.clear();
    out.reserve(raw_path.size());

    size_t pos = 0;
    for (;;) {
        const size_t slash = raw_path.find('/', pos);
        const std::string_view segment =
            raw_path.substr(pos, slash == std::string_view::npos ? std::string_view::npos
                                                                 : slash - pos);

        std::string decoded;
        decoded.reserve(segment.size());
        bool had_escape = false;

        for (size_t i = 0; i < segment.size(); ++i) {
            if (segment[i] != '%') {
                decoded += segment[i];
                continue;
            }
            if (i + 2 >= segment.size()) {
                return false;  // truncated escape
            }
            const int hi = hexValue(static_cast<unsigned char>(segment[i + 1]));
            const int lo = hexValue(static_cast<unsigned char>(segment[i + 2]));
            if (hi < 0 || lo < 0) {
                return false;  // not hex
            }
            const auto byte = static_cast<unsigned char>(hi * 16 + lo);
            if (byte < 0x20 || byte == 0x7F) {
                return false;  // control character, NUL included
            }
            if (byte == '/') {
                return false;  // would manufacture a segment boundary
            }
            decoded += static_cast<char>(byte);
            had_escape = true;
            i += 2;
        }

        // Only refuse what decoding produced. A literal ".." segment is the
        // caller's business, and was always passed through.
        if (had_escape && decoded == "..") {
            return false;
        }

        out += decoded;
        if (slash == std::string_view::npos) {
            break;
        }
        out += '/';
        pos = slash + 1;
    }

    return true;
}

}  // namespace caduvelox::url
