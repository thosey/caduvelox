#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>

namespace caduvelox {

/**
 * Utility class for creating and managing SSL contexts optimized for kTLS.
 * 
 * This helper ensures the SSL context is properly configured for kernel TLS:
 * - TLS 1.2 only (required for kTLS)
 * - AES128-GCM-SHA256 cipher only (supported by kTLS)
 * - SSL_OP_ENABLE_KTLS enabled globally
 */
class KTLSContextHelper {
public:
    /**
     * Create an SSL context configured for kTLS server operations.
     * 
     * @param cert_path Path to the PEM certificate file
     * @param key_path Path to the PEM private key file
     * @return SSL_CTX* on success, nullptr on failure
     */
    static SSL_CTX* createServerContext(const std::string& cert_path, const std::string& key_path);

    /**
     * Clean up and free an SSL context.
     * 
     * @param ctx SSL context to free (can be nullptr)
     */
    static void freeContext(SSL_CTX* ctx);

private:
    static bool initializeOpenSSL();
    static bool loadCertificates(SSL_CTX* ctx, const std::string& cert_path, const std::string& key_path);
};

} // namespace caduvelox