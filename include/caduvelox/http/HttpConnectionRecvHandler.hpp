#pragma once

#include "caduvelox/util/ProvidedBufferToken.hpp"
#include <cstdint>

namespace caduvelox {

// Forward declaration
class HttpConnectionJob;

/**
 * Handler for HttpConnectionJob multishot recv operations with inline processing.
 *
 * Holds a raw pointer plus the connection's pool generation at arm time. The
 * connection can be freed while the multishot recv is still armed (e.g. when
 * closeConnection() could not submit a cancel due to pool/SQE exhaustion —
 * closing the fd does NOT terminate an in-flight io_uring recv, which holds
 * its own file reference). Both callbacks validate (connection, generation)
 * via PoolManager before dereferencing, so late completions against a freed
 * or recycled connection are dropped instead of causing use-after-free.
 */
struct HttpConnectionRecvHandler {
    HttpConnectionJob* connection;
    uint64_t connection_gen;  // pool generation of connection at arm time

    // Error handling
    void onError(int error);

    // Zero-copy token processing (inline on io_uring thread)
    // Token passed by reference - lifecycle managed by caller's scope
    void onDataToken(ProvidedBufferToken& token);
};

} // namespace caduvelox
