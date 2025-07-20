#include "caduvelox/jobs/KTLSContextHelper.hpp"
#include "caduvelox/logger/Logger.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <filesystem>

namespace caduvelox {

SSL_CTX* KTLSContextHelper::createServerContext(const std::string& cert_path, const std::string& key_path) {
    // Initialize OpenSSL
    if (!initializeOpenSSL()) {
        return nullptr;
    }

    // Create SSL context
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        Logger::getInstance().logError("KTLSContextHelper: Failed to create SSL context");
        return nullptr;
    }

    // Enable kTLS globally for this context
    SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
    Logger::getInstance().logMessage("KTLSContextHelper: Enabled SSL_OP_ENABLE_KTLS globally on SSL_CTX");

    // Use TLS 1.2 as minimum but allow TLS 1.3 if kernel supports it
    // This provides broader compatibility while still prioritizing kTLS
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    Logger::getInstance().logMessage("KTLSContextHelper: Configured for TLS 1.2+ with kTLS preference");

    // Use a more permissive cipher list that includes kTLS-compatible ciphers
    // but falls back to others if needed for compatibility
    if (!SSL_CTX_set_cipher_list(ctx, "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-CHACHA20-POLY1305:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5")) {
        Logger::getInstance().logError("KTLSContextHelper: Failed to set cipher list");
        SSL_CTX_free(ctx);
        return nullptr;
    }

    // Load certificates
    if (!loadCertificates(ctx, cert_path, key_path)) {
        SSL_CTX_free(ctx);
        return nullptr;
    }

    Logger::getInstance().logMessage("KTLSContextHelper: SSL context created successfully with kTLS configuration");
    return ctx;
}

void KTLSContextHelper::freeContext(SSL_CTX* ctx) {
    if (ctx) {
        SSL_CTX_free(ctx);
    }
}

bool KTLSContextHelper::initializeOpenSSL() {
    // Initialize OpenSSL
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    return true;
}

bool KTLSContextHelper::loadCertificates(SSL_CTX* ctx, const std::string& cert_path, const std::string& key_path) {
    Logger& logger = Logger::getInstance();
    namespace fs = std::filesystem;

    // Prepare candidate certificate paths (try given path, then common test locations)
    const std::string cert_filename = fs::path(cert_path).filename().string();
    const fs::path cert_candidates[] = {
        fs::path(cert_path),
        fs::path("tests") / cert_filename,
        fs::path("../tests") / cert_filename
    };

    bool cert_loaded = false;
    for (const auto& p : cert_candidates) {
        if (fs::exists(p)) {
            if (SSL_CTX_use_certificate_file(ctx, p.string().c_str(), SSL_FILETYPE_PEM) == 1) {
                cert_loaded = true;
                break;
            } else {
                logger.logError("KTLSContextHelper: Failed to load certificate from: " + p.string());
            }
        }
    }
    if (!cert_loaded) {
        logger.logError("KTLSContextHelper: Unable to load certificate from provided or fallback locations");
        return false;
    }

    // Prepare candidate key paths
    const std::string key_filename = fs::path(key_path).filename().string();
    const fs::path key_candidates[] = {
        fs::path(key_path),
        fs::path("tests") / key_filename,
        fs::path("../tests") / key_filename
    };

    bool key_loaded = false;
    for (const auto& p : key_candidates) {
        if (fs::exists(p)) {
            if (SSL_CTX_use_PrivateKey_file(ctx, p.string().c_str(), SSL_FILETYPE_PEM) == 1) {
                key_loaded = true;
                break;
            } else {
                logger.logError("KTLSContextHelper: Failed to load private key from: " + p.string());
            }
        }
    }
    if (!key_loaded) {
        logger.logError("KTLSContextHelper: Unable to load private key from provided or fallback locations");
        return false;
    }

    // Verify that the private key matches the certificate
    if (!SSL_CTX_check_private_key(ctx)) {
        logger.logError("KTLSContextHelper: Private key does not match the certificate public key");
        return false;
    }

    logger.logMessage("KTLSContextHelper: Certificates loaded successfully");
    return true;
}

} // namespace caduvelox
