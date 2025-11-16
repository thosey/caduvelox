#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/threading/AffinityWorkerPool.hpp"

namespace caduvelox {

// Error handling
void HttpConnectionRecvHandler::onError(int error) {
    // Try to get shared_ptr from weak_ptr
    auto conn_shared = connection.lock();
    if (!conn_shared) {
        // Connection already destroyed, nothing to do
        return;
    }
    
    conn_shared->handleReadError(error);
}

// Zero-copy token processing (with worker threads)
void HttpConnectionRecvHandler::onDataToken(std::shared_ptr<ProvidedBufferToken> token, 
                                           void* worker_pool_ptr) {
    // Try to get shared_ptr from weak_ptr
    auto conn_shared = connection.lock();
    if (!conn_shared) {
        // Connection already destroyed, don't post to worker
        return;
    }
    
    auto* worker_pool = static_cast<AffinityWorkerPool*>(worker_pool_ptr);
    int connection_id = conn_shared->getClientFd();
    
    // Post to worker thread with connection affinity
    // Capture shared_ptr to keep connection alive - NO MORE RAW POINTERS!
    worker_pool->post(connection_id, [conn_shared, token]() {
        conn_shared->handleDataReceivedOnWorker(token->data(), token->size());
    });
}

} // namespace caduvelox