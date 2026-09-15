#pragma once
#include "caduvelox/http/HttpResponse.hpp"
#include <cstdint>
#include <string>

namespace caduvelox {

/**
 * Build the status line and header block for a response -- the one place a
 * response head becomes bytes.
 *
 * HttpResponse::headers is a public map, so by the time a response reaches the
 * wire it holds whatever a handler chose to put there. Anything accepted here
 * that a cache or proxy in front of this server would read differently is a
 * response-desync primitive, so this is deliberately strict:
 *
 *   - Content-Length is written from `content_length`, the size of what is
 *     actually being sent. Any content-length in the map is ignored: a value
 *     that disagrees with the body frames the message wrong even when there is
 *     only one of it.
 *   - Field names are written in lowercase, so two spellings of one field
 *     cannot both go out. Two spellings with different values are refused.
 *   - Transfer-Encoding is refused. This server applies no transfer codings,
 *     so the header could only contradict Content-Length.
 *   - Field names and values must satisfy the same RFC 9110 syntax the request
 *     parser enforces, and the reason phrase must be RFC 9112 reason-phrase.
 *     Above all this means no CR or LF, either of which lets a value or the
 *     status text start a header line of its own.
 *   - The status code must be 100-599.
 *
 * @param content_length Length of the body that will follow the head.
 * @return false if the response cannot be written safely; `out` is then
 *         unspecified, and the caller should send fallback_error_response().
 */
bool build_response_head(const HttpResponse& res, uint64_t content_length, std::string& out);

/**
 * A fixed, known-good 500, for when build_response_head() refuses a response.
 * @param close_connection Whether to advertise "connection: close".
 */
std::string fallback_error_response(bool close_connection);

} // namespace caduvelox
