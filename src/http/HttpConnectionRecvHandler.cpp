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
void HttpConnectionRecvHandler::onDataToken(ProvidedBufferToken& token) {
    // Check if connection is still valid
    if (!connection) {
        return;  
    }
    
    connection->handleDataReceived(token.data(), token.size());
}

} // namespace caduvelox