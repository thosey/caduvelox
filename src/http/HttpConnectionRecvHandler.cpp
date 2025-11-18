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
    
    auto* worker_pool = static_cast<AffinityWorkerPool*>(worker_pool_ptr);
    int connection_id = connection->getClientFd();
    
    // Post to worker thread with connection affinity
    // Worker will use tryAcquire/tryRelease for safe access
    HttpConnectionJob* conn_ptr = connection;
    worker_pool->post(connection_id, [conn_ptr, token]() {
        conn_ptr->handleDataReceivedOnWorker(token->data(), token->size());
    });
}

} // namespace caduvelox