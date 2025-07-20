#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace caduvelox {

template<typename HeaderMap>
std::string read_header(const HeaderMap &headers, const std::string &name);

/**
 * HTTP response representation.
 * Headers are stored in lowercase for case-insensitive lookup.
 */
struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string file_path;
    std::string body;

    std::string getHeader(const std::string &name) const { return read_header(headers, name); }

    // Convenience methods
    void setStatus(int code, const std::string &text = "") {
        status_code = code;
        status_text = text.empty() ? getDefaultStatusText(code) : text;
    }

    void setHeader(const std::string &name, const std::string &value) {
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        headers[key] = value;
    }

    void setContentType(const std::string &content_type) {
        setHeader("content-type", content_type);
    }

    void setBody(const std::string &content) {
        body = content;
        setHeader("content-length", std::to_string(body.size()));
    }

    void setFile(const std::string &path) {
        file_path = path;
    }

    // Convenience methods for common response types
    void html(const std::string &content) {
        setContentType("text/html");
        setBody(content);
    }

    void json(const std::string &content) {
        setContentType("application/json");
        setBody(content);
    }

    void sendFile(const std::string &path) {
        file_path = path;
        // NOTE: file_path is internal only - the server reads this field directly
        // and does NOT send it as a header to avoid leaking filesystem information
        // Content-Type will be set based on file extension by the server
    }

private:
    static std::string getDefaultStatusText(int code) {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 413: return "Payload Too Large";
            case 415: return "Unsupported Media Type";
            case 422: return "Unprocessable Entity";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            default: return "Unknown";
        }
    }
};

} // namespace caduvelox
