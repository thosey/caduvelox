#include "caduvelox/http/HttpConnectionRecvHandler.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/threading/AffinityWorkerPool.hpp"

namespace caduvelox {

// Error handling
void HttpConnectionRecvHandler::onError(int error) {
    connection->handleReadError(error);
}

// Zero-copy token processing (with worker threads)
void HttpConnectionRecvHandler::onDataToken(std::shared_ptr<ProvidedBufferToken> token, 
                                           void* worker_pool_ptr) {
    auto* worker_pool = static_cast<AffinityWorkerPool*>(worker_pool_ptr);
    int connection_id = connection->getClientFd();
    
    // Post to worker thread with connection affinity
    worker_pool->post(connection_id, [conn = connection, token]() {
        conn->handleDataReceivedOnWorker(token->data(), token->size());
    });
}

} // namespace caduvelox