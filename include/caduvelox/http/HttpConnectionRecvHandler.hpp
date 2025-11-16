#pragma once

#include "caduvelox/util/ProvidedBufferToken.hpp"
#include <memory>

namespace caduvelox {

// Forward declarations
class HttpConnectionJob;
class AffinityWorkerPool;

/**
 * Handler for HttpConnectionJob multishot recv operations with affinity workers
 * Uses shared_ptr to keep connection alive while multishot recv is active
 */
struct HttpConnectionRecvHandler {
    std::weak_ptr<HttpConnectionJob> connection;  // weak_ptr to avoid circular reference
    
    // Error handling
    void onError(int error);
    
    // Zero-copy token processing (with worker threads)
    void onDataToken(std::shared_ptr<ProvidedBufferToken> token, void* worker_pool_ptr);
};

} // namespace caduvelox
