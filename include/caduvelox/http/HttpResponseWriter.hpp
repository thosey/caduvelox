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
 *   - Content-Length is omitted entirely for a status that cannot carry a body
 *     (see status_allows_body); it used to be written on every response, 204
 *     included.
 *   - An empty status_text is filled in from the status code. The two are
 *     independent public fields, so `res.status_code = 404` alone used to emit
 *     "HTTP/1.1 404 OK".
 *
 * @param content_length Length of the body that will follow the head.
 * @return false if the response cannot be written safely; `out` is then
 *         unspecified, and the caller should send fallback_error_response().
 */
bool build_response_head(const HttpResponse& res, uint64_t content_length, std::string& out);

/**
 * May a response with this status carry a body?
 *
 * No for 1xx, 204 and 304. RFC 9110 section 8.6 forbids Content-Length on a 204,
 * and none of these may have a body at all -- so a caller must not append one
 * either, or the bytes would be read as the start of the next response.
 */
bool status_allows_body(int status_code);

/**
 * A fixed, known-good 500, for when build_response_head() refuses a response.
 * @param close_connection Whether to advertise "connection: close".
 */
std::string fallback_error_response(bool close_connection);

} // namespace caduvelox
