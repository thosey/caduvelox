#pragma once

#include <cstddef>
#include <cstdint>

namespace caduvelox::ring_limits {

// Limits on io_uring dimensions, shared by ServerConfig::validate() and
// Server::init() so the two cannot disagree. Kept free of liburing so the config
// header can include it.
//
// Both measured on Linux 7.2 rather than taken on trust:
//
//   io_uring_setup_buf_ring   accepts powers of two from 1 to 32768 entries and
//                             fails with EINVAL on anything else, including
//                             non-powers such as 500 and anything above 32768
//   io_uring_queue_init       accepts 1 to 32768 and rounds a non-power up, so
//                             300 is fine; 0 and 32769 fail with EINVAL
inline constexpr unsigned MAX_BUFFER_RING_ENTRIES = 32768;
inline constexpr unsigned MAX_QUEUE_DEPTH = 32768;

inline constexpr bool isValidBufferRingCount(unsigned n) {
    return n != 0 && (n & (n - 1)) == 0 && n <= MAX_BUFFER_RING_ENTRIES;
}

// The kernel's per-buffer length is 32 bits (struct io_uring_buf::len is __u32,
// and io_uring_buf_ring_add() takes an unsigned int). A larger size is silently
// truncated on the way in. Worse, the coordinator allocates count * size bytes,
// and with a size_t that large the product can wrap: count 32768 with size
// 2^49 + 1 wraps to 32 KiB, which mmap happily provides, and every buffer after
// the first then points outside it -- where recv would write network data.
// Measured: init() reported success. Capping at the field's width keeps the
// length honest and bounds the product at about 2^47, so it cannot wrap.
inline constexpr std::size_t MAX_BUFFER_SIZE = UINT32_MAX;

inline constexpr bool isValidBufferSize(std::size_t n) {
    return n != 0 && n <= MAX_BUFFER_SIZE;
}

inline constexpr bool isValidQueueDepth(unsigned n) {
    return n >= 1 && n <= MAX_QUEUE_DEPTH;
}

}  // namespace caduvelox::ring_limits
