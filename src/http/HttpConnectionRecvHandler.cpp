#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/http/SingleRingHttpServer.hpp"
#include "caduvelox/util/PoolManager.hpp"

namespace caduvelox {

// Error handling
void HttpConnectionRecvHandler::onError(int error) {
    // Drop the completion if the connection was freed (or its pool slot
    // recycled) while this recv stayed armed.
    if (connection && PoolManager::isValid(connection, connection_gen)) {
        connection->handleReadError(error);
    }
}

// Zero-copy token processing (inline on io_uring thread)
void HttpConnectionRecvHandler::onDataToken(ProvidedBufferToken& token) {
    // Drop the completion if the connection was freed (or its pool slot
    // recycled) while this recv stayed armed. The token still recycles its
    // buffer when it goes out of scope in the caller.
    if (!connection || !PoolManager::isValid(connection, connection_gen)) {
        return;
    }

    connection->handleDataReceived(token.data(), token.size());
}

bool HttpConnectionRecvHandler::shouldRearmRecv() {
    // Same validity gate as the other callbacks: a freed or recycled connection
    // must never be revived, and nothing should be re-armed on its behalf.
    if (!connection || !PoolManager::isValid(connection, connection_gen)) {
        return false;
    }

    return connection->wantsContinuedRead();
}

} // namespace caduvelox