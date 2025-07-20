#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace caduvelox {

template<typename HeaderMap>
std::string read_header(const HeaderMap &headers, const std::string &name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = headers.find(key);
    return it != headers.end() ? it->second : "";
}

/**
 * HTTP request representation.
 * Headers are stored in lowercase for case-insensitive lookup.
 */
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // Helper methods
    std::string getHeader(const std::string &name) const { return read_header(headers, name); }
};

} // namespace caduvelox
