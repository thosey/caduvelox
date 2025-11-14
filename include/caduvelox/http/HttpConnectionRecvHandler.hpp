#pragma once

#include "caduvelox/util/ProvidedBufferToken.hpp"
#include <memory>

namespace caduvelox {

// Forward declarations
class HttpConnectionJob;
class AffinityWorkerPool;

/**
 * Handler for HttpConnectionJob multishot recv operations with affinity workers
 * Uses raw pointer - worker thread will use tryAcquire/tryRelease for safety
 */
struct HttpConnectionRecvHandler {
    HttpConnectionJob* connection;  // Raw pointer - protected by atomic state machine
    
    // Error handling
    void onError(int error);
    
    // Zero-copy token processing (with worker threads)
    void onDataToken(std::shared_ptr<ProvidedBufferToken> token, void* worker_pool_ptr);
};

} // namespace caduvelox
