#pragma once

#include <string>

namespace caduvelox::port {

// A TCP port as bind(2) accepts it. Zero is legal and means "any free port".
inline constexpr int MIN = 0;
inline constexpr int MAX = 65535;

inline constexpr bool isValid(int port) {
    return port >= MIN && port <= MAX;
}

/**
 * Why a port was refused.
 *
 * Shared so the single-ring and multi-ring paths cannot disagree. The multi-ring
 * path had no check at all: sockaddr_in::sin_port is 16 bits, so htons() silently
 * truncated anything larger and the server bound a port nobody asked for --
 * measured, 70000 became 4464, -1 became 65535, and 65536 became 0, which bind(2)
 * reads as "any free port".
 */
inline std::string rejection(int port) {
    return "Invalid port " + std::to_string(port) + " (must be between " +
           std::to_string(MIN) + " and " + std::to_string(MAX) + ")";
}

}  // namespace caduvelox::port
