#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/threading/AffinityWorkerPool.hpp"

namespace caduvelox {

// Error handling
void HttpConnectionRecvHandler::onError(int error) {
    auto conn = connection.lock();
    if (conn) {
        conn->handleReadError(error);
    }
    // If connection expired, silently ignore - connection already closed
}

// Zero-copy token processing (with worker threads)
void HttpConnectionRecvHandler::onDataToken(std::shared_ptr<ProvidedBufferToken> token, 
                                           void* worker_pool_ptr) {
    // Lock weak_ptr to get shared_ptr - this keeps connection alive during worker processing
    auto conn = connection.lock();
    if (!conn) {
        // Connection was closed before worker could process - discard token
        return;
    }
    
    auto* worker_pool = static_cast<AffinityWorkerPool*>(worker_pool_ptr);
    int connection_id = conn->getClientFd();
    
    // Post to worker thread with connection affinity
    // Capture shared_ptr in lambda to keep connection alive until processing completes
    worker_pool->post(connection_id, [conn, token]() {
        conn->handleDataReceivedOnWorker(token->data(), token->size());
    });
}

} // namespace caduvelox