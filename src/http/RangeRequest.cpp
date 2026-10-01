#include "caduvelox/http/RangeRequest.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <limits>

namespace caduvelox::http {

namespace {

std::string_view trim_ows(std::string_view s) {
    const size_t first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const size_t last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// Parse one run of DIGITs as a uint64. Refuses an empty run, a non-digit, and
// anything that would not fit -- a client can write 30 nines, and letting that
// wrap would turn "past the end of the file" into a small offset inside it.
bool parse_digits(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (kMax - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

} // namespace

std::optional<ResolvedRange> ByteRangeSpec::resolve(uint64_t file_size) const {
    if (suffix) {
        // "bytes=-N": the final N bytes. N larger than the file is not an
        // error -- RFC 9110 section 14.1.2 says the whole representation is
        // then the answer -- but N of zero names no bytes at all, and neither
        // does any range over an empty file.
        if (first == 0 || file_size == 0) return std::nullopt;
        const uint64_t offset = (first >= file_size) ? 0 : file_size - first;
        return ResolvedRange{offset, file_size - offset};
    }

    // A first-byte-position *at* the end is already past the last byte, so
    // ">=" rather than ">". This also rejects every range over a zero-byte
    // file, which is correct: it has no byte 0 to start at.
    if (first >= file_size) return std::nullopt;

    const uint64_t last_byte = std::min(last.value_or(file_size - 1), file_size - 1);
    return ResolvedRange{first, last_byte - first + 1};
}

std::optional<ByteRangeSpec> parse_range(std::string_view field_value) {
    std::string_view value = trim_ows(field_value);
    if (value.empty()) return std::nullopt;

    const size_t eq = value.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    if (!equals_ignore_case(trim_ows(value.substr(0, eq)), "bytes")) return std::nullopt;

    std::string_view range_set = trim_ows(value.substr(eq + 1));

    // More than one range asked for: serve the whole representation. See the
    // header for why this server does not do multipart/byteranges.
    if (range_set.find(',') != std::string_view::npos) return std::nullopt;

    const size_t dash = range_set.find('-');
    if (dash == std::string_view::npos) return std::nullopt;

    ByteRangeSpec spec;
    if (dash == 0) {
        // suffix-range: "-" suffix-length, with no first-pos.
        if (!parse_digits(range_set.substr(1), spec.first)) return std::nullopt;
        spec.suffix = true;
        return spec;
    }

    // int-range: first-pos "-" [ last-pos ]
    if (!parse_digits(range_set.substr(0, dash), spec.first)) return std::nullopt;

    const std::string_view last_pos = range_set.substr(dash + 1);
    if (!last_pos.empty()) {
        uint64_t last = 0;
        if (!parse_digits(last_pos, last)) return std::nullopt;
        // A last-pos below first-pos is not unsatisfiable, it is invalid, and
        // an invalid field is ignored rather than refused.
        if (last < spec.first) return std::nullopt;
        spec.last = last;
    }
    return spec;
}

std::string format_http_date(int64_t unix_seconds) {
    static constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    if (gmtime_r(&t, &tm) == nullptr) return {};
    if (tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11) return {};

    char buf[40];
    const int n = std::snprintf(buf, sizeof(buf),
                                "%s, %02d %s %04d %02d:%02d:%02d GMT",
                                kDays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon],
                                tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) return {};
    return std::string(buf, static_cast<size_t>(n));
}

} // namespace caduvelox::http
