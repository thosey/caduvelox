#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace caduvelox::http {

/**
 * A byte range resolved against a known representation length.
 */
struct ResolvedRange {
    uint64_t offset;  // First byte to send.
    uint64_t length;  // Number of bytes to send; never 0 for a satisfiable range.
};

/**
 * A single byte range as the client wrote it, before the file size is known.
 *
 * Resolution has to be deferred: "bytes=-500" means "the last 500 bytes", which
 * is not an offset until something has stat()ed the file, and the only place
 * that happens is HTTPFileJob::openFile(). Parsing at the connection and
 * resolving at the file also means one fstat(2) decides both the Content-Range
 * and the bytes spliced, so the two cannot disagree.
 */
struct ByteRangeSpec {
    // True for the suffix form "bytes=-N", where `first` holds N -- a count of
    // bytes from the end, not a position. `last` is then never set.
    bool suffix = false;
    uint64_t first = 0;
    std::optional<uint64_t> last;

    /**
     * Turn this into an offset and a length against a representation of
     * `file_size` bytes.
     *
     * @return nullopt when the range cannot be met, which is a 416 whose
     *         Content-Range reports the real length -- not a 404, and not a
     *         silent whole-file 200. Every range over a zero-byte file lands
     *         here, as does "bytes=-0", which asks for the last nothing bytes.
     */
    std::optional<ResolvedRange> resolve(uint64_t file_size) const;
};

/**
 * Parse a Range request header field value.
 *
 * Only a *single* "bytes" range is honoured. A multi-range request
 * ("bytes=0-99,200-299") is answered with the whole representation instead: a
 * correct reply needs a multipart/byteranges body with generated boundaries and
 * one splice per part, and RFC 9110 section 14.2 explicitly permits a server to
 * ignore the field. Returning the whole thing is always a correct answer; a
 * truncated single part dressed up as the full multi-range answer would not be.
 *
 * Deliberately conflates "no Range field" with "a Range field this server will
 * not act on", because the two have the same correct outcome. That includes a
 * range unit other than bytes, and anything malformed -- RFC 9110 section 14.2
 * requires a malformed field to be ignored rather than refused, so a stray
 * character cannot turn a servable request into an error.
 *
 * Unsatisfiability is *not* decided here. "bytes=99999-" is well-formed, and
 * whether it can be met depends on a file this function has never seen; see
 * ByteRangeSpec::resolve().
 *
 * @param field_value The Range field value, e.g. "bytes=0-499".
 * @return The range to serve, or nullopt to serve the whole representation.
 */
std::optional<ByteRangeSpec> parse_range(std::string_view field_value);

/**
 * Format a time_t as an IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT",
 * which is the one date format RFC 9110 section 5.6.7 requires a server to send.
 *
 * Written out by hand rather than with strftime(3) because %a and %b there are
 * locale-dependent: under a non-English LC_TIME the same call emits day and
 * month names no HTTP client is required to parse. gmtime_r(3) rather than
 * gmtime(3) for the usual reason -- gmtime returns a pointer to one shared
 * static struct tm, so two ring threads formatting a date at once corrupt each
 * other's result.
 */
std::string format_http_date(int64_t unix_seconds);

} // namespace caduvelox::http
