#pragma once

#include <string>
#include <string_view>

namespace caduvelox::url {

/**
 * Percent-decode a request path, one segment at a time.
 *
 * Nothing in this framework used to decode anything, which made it safe against
 * %2e%2e by accident and unable to serve a file called "a b.txt" by the same
 * accident -- a browser asks for that as "a%20b.txt", and the lookup failed.
 *
 * Decoding gives up that accidental safety, so the rules below replace it. The
 * path is split on '/' FIRST and each segment decoded separately, because the
 * classic bypass is decoding first: "%2f" then becomes a separator, and
 * "..%2f..%2fsecret" turns into traversal that no ".." check ever sees.
 *
 * A path is refused (false) when:
 *   - an escape is truncated ("%2", "%") or not hex ("%zz");
 *   - an escape produces a control character, NUL included -- a NUL would
 *     truncate the name for any C API downstream;
 *   - an escape produces '/', which would manufacture a segment boundary;
 *   - an escape produces a segment that reads "..", which would manufacture a
 *     parent-directory step.
 *
 * A *literal* ".." segment is passed through unchanged, exactly as before: it is
 * visible to whoever built the route, and confining a path to a document root
 * remains their decision (see the weakly_canonical() check in
 * examples/static_https_server). Decoding is not allowed to create one, which is
 * the new part.
 *
 * "%252e" decodes once, to the three characters "%2e", and is not decoded again;
 * double-encoding therefore cannot reach "..".
 *
 * '+' is left alone. It means space in a query string, never in a path.
 *
 * @param raw_path Path portion of the request target, without the query string.
 * @param out      Decoded path on success; unspecified on failure.
 * @return false if the path must be refused.
 */
bool decodePath(std::string_view raw_path, std::string& out);

}  // namespace caduvelox::url
