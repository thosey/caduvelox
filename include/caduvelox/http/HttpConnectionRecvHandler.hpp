#pragma once

#include "caduvelox/util/ProvidedBufferToken.hpp"
#include <memory>

namespace caduvelox {

// Forward declaration
class HttpConnectionJob;

/**
 * Handler for HttpConnectionJob multishot recv operations with inline processing
 * Uses raw pointer - connection job handles lifecycle
 */
struct HttpConnectionRecvHandler {
    HttpConnectionJob* connection;  // Raw pointer - protected by atomic state machine
    
    // Error handling
    void onError(int error);
    
    // Zero-copy token processing (inline on io_uring thread)
    void onDataToken(std::shared_ptr<ProvidedBufferToken> token);
};

} // namespace caduvelox
