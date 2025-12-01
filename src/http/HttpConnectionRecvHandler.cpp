#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"

namespace caduvelox {

// Error handling
void HttpConnectionRecvHandler::onError(int error) {
    // Raw pointer access - connection might be discarded
    // handleReadError will check state if needed
    if (connection) {
        connection->handleReadError(error);
    }
}

// Zero-copy token processing (inline on io_uring thread)
void HttpConnectionRecvHandler::onDataToken(std::shared_ptr<ProvidedBufferToken> token) {
    // Check if connection is still valid
    if (!connection) {
        return;
    }
    
    // OPTIMIZATION: Process HTTP request directly on io_uring thread
    // This eliminates cross-thread ping-pong that was killing performance:
    // Old flow: io_uring -> worker (parse/route) -> io_uring (send)
    // New flow: io_uring (recv/parse/route/send) - all on same thread
    //
    // Benefits:
    // - No thread context switches (was ~40ms latency per request)
    // - No queue contention between workers and io_uring
    // - Better cache locality
    // - Simpler code path
    //
    // Risks:
    // - HTTP processing blocks io_uring event loop
    // - For slow handlers (database queries, external APIs), this could hurt throughput
    //
    // Mitigation:
    // - Keep HTTP handlers fast (< 100μs)
    // - For slow operations, use async I/O (io_uring for disk, async network)
    // - Static file serving is perfect for this (just io_uring sendfile)
    connection->handleDataReceived(token->data(), token->size());
}

} // namespace caduvelox