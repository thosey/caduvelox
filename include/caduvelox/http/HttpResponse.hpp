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
    // Empty means "derive it from status_code when the response is written". The
    // two fields are independent, and only setStatus() keeps them in step, so a
    // handler writing `res.status_code = 404` used to emit "HTTP/1.1 404 OK".
    // Defaulting to empty makes that case right instead of wrong.
    std::string status_text;
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
        const std::string key = lowercase(name);
        // Drop any other spelling of this field written straight into the map,
        // or this would add a second copy of the field instead of replacing it.
        for (auto it = headers.begin(); it != headers.end();) {
            if (it->first != key && lowercase(it->first) == key) {
                it = headers.erase(it);
            } else {
                ++it;
            }
        }
        headers[key] = value;
    }

    /**
     * Fold every header name to lowercase in place.
     *
     * headers is a public map, and a handler can write "Content-Type" into it
     * directly. Anything that then looks for "content-type" misses it and adds a
     * default alongside -- so this has to run before defaults are filled in,
     * which is where the router and HTTPFileJob call it.
     *
     * @return false if two spellings of one field carried different values. The
     *         first one seen is kept, but the response should not be sent.
     */
    bool normalizeHeaders() {
        std::unordered_map<std::string, std::string> normalized;
        normalized.reserve(headers.size());
        bool consistent = true;
        for (auto &[name, value] : headers) {
            // try_emplace leaves `value` untouched when it does not insert, so it
            // is still readable for the comparison.
            auto [it, inserted] = normalized.try_emplace(lowercase(name), std::move(value));
            if (!inserted && it->second != value) {
                consistent = false;
            }
        }
        headers = std::move(normalized);
        return consistent;
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

    /**
     * The reason phrase for a status code -- "Not Found" for 404.
     *
     * Public because the response writer uses it to fill in an empty status_text.
     * An unknown code gets "Unknown"; the phrase is advisory (RFC 9112 section 4),
     * so anything printable will do.
     */
    static std::string getDefaultStatusText(int code) {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 206: return "Partial Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 413: return "Content Too Large";
            case 414: return "URI Too Long";
            case 416: return "Range Not Satisfiable";
            case 415: return "Unsupported Media Type";
            case 422: return "Unprocessable Entity";
            case 429: return "Too Many Requests";
            case 431: return "Request Header Fields Too Large";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            default: return "Unknown";
        }
    }

private:
    static std::string lowercase(const std::string &s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

};

} // namespace caduvelox
