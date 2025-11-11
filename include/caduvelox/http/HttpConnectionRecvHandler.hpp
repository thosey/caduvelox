#pragma once

#include "caduvelox/util/ProvidedBufferToken.hpp"
#include <memory>

namespace caduvelox {

// Forward declarations
class HttpConnectionJob;
class AffinityWorkerPool;

/**
 * Handler for HttpConnectionJob multishot recv operations with affinity workers
 * Uses weak_ptr to safely handle connection lifetime across worker threads
 */
struct HttpConnectionRecvHandler {
    std::weak_ptr<HttpConnectionJob> connection;
    
    // Error handling
    void onError(int error);
    
    // Zero-copy token processing (with worker threads)
    void onDataToken(std::shared_ptr<ProvidedBufferToken> token, void* worker_pool_ptr);
};

} // namespace caduvelox
