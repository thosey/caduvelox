#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/threading/AffinityWorkerPool.hpp"

namespace caduvelox {

// Error handling
void HttpConnectionRecvHandler::onError(int error) {
    // Raw pointer access - connection might be discarded
    // handleReadError will check state if needed
    if (connection) {
        connection->handleReadError(error);
    }
}

// Zero-copy token processing (with worker threads)
void HttpConnectionRecvHandler::onDataToken(std::shared_ptr<ProvidedBufferToken> token, 
                                           void* worker_pool_ptr) {
    // Check if connection is still valid
    if (!connection) {
        return;
    }
    
    // Get shared_ptr to keep connection alive on worker thread
    auto conn_shared = connection->getShared();
    if (!conn_shared) {
        // Connection is being destroyed, don't post to worker
        return;
    }
    
    auto* worker_pool = static_cast<AffinityWorkerPool*>(worker_pool_ptr);
    int connection_id = connection->getClientFd();
    
    // Post to worker thread with connection affinity
    // Capture shared_ptr to keep connection alive - NO MORE RAW POINTERS!
    worker_pool->post(connection_id, [conn_shared, token]() {
        conn_shared->handleDataReceivedOnWorker(token->data(), token->size());
    });
}

} // namespace caduvelox