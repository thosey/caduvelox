#include <gtest/gtest.h>
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <thread>
#include <chrono>

using namespace caduvelox;

/**
 * Test for Issue #2: SSL_CTX Double-Free in Multi-Ring HTTPS
 * 
 * Verifies that the shared SSL_CTX is not freed multiple times during
 * multi-ring server shutdown.
 * 
 * Expected Result: This test should FAIL (crash or be detected by AddressSanitizer)
 * before the fix, showing heap corruption from double-free.
 * 
 * Run with: ASAN_OPTIONS=detect_leaks=1 ctest -R MultiRingKTLSShutdownTest
 */
class MultiRingKTLSShutdownTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
    }
};

TEST_F(MultiRingKTLSShutdownTest, NoDoubleFreeOnShutdown) {
    const std::string cert_path = "test_cert.pem";
    const std::string key_path = "test_key.pem";
    const int num_rings = 3;  // Use multiple rings to expose the issue
    const int test_port = 8445;
    
    std::cout << "Creating multi-ring HTTPS server with " << num_rings << " rings..." << std::endl;
    
    // Create and start multi-ring server
    {
        HttpServer server(num_rings, 128);
        
        // Add a simple route
        server.addRoute("GET", "/test", [](const HttpRequest& req, HttpResponse& res) {
            res.setStatus(200);
            res.setBody("Test response");
        });
        
        // Start KTLS - this creates one SSL_CTX and shares it with all rings
        bool started = server.listenKTLS(test_port, cert_path, key_path);
        
        if (!started) {
            std::cout << "Note: Failed to start KTLS server. "
                      << "This test requires TLS certificates and kernel KTLS support." << std::endl;
            GTEST_SKIP() << "KTLS not available, skipping test";
        }
        
        std::cout << "Server started successfully" << std::endl;
        
        // Let it run briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Stop the server
        std::cout << "Stopping server..." << std::endl;
        server.stop();
        
        std::cout << "Server stopped, about to destroy..." << std::endl;
        
        // Destructor runs here - this is where the double-free occurs
        // With AddressSanitizer enabled, this should detect:
        // - N frees from SingleRingHttpServer destructors (one per ring)
        // - 1 free from HttpServer destructor
        // Total: N+1 frees of the same SSL_CTX*
    }
    
    std::cout << "Server destroyed successfully (no crash detected)" << std::endl;
    
    // If we reach here without crashing, the test passes
    // However, AddressSanitizer should still report the double-free
    SUCCEED();
}

TEST_F(MultiRingKTLSShutdownTest, MultipleStartStopCycles) {
    const std::string cert_path = "test_cert.pem";
    const std::string key_path = "test_key.pem";
    const int num_rings = 4;
    
    // Run multiple start/stop cycles to amplify the issue
    for (int cycle = 0; cycle < 3; cycle++) {
        std::cout << "Cycle " << cycle << ": Creating server..." << std::endl;
        
        HttpServer server(num_rings, 128);
        server.addRoute("GET", "/", [](const HttpRequest&, HttpResponse& res) {
            res.setStatus(200);
        });
        
        // Use different ports for each cycle to avoid bind errors
        int port = 8450 + cycle;
        bool started = server.listenKTLS(port, cert_path, key_path);
        
        if (!started) {
            GTEST_SKIP() << "KTLS not available";
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        std::cout << "Cycle " << cycle << ": Stopping server..." << std::endl;
        server.stop();
        
        // Each destruction cycle has N+1 frees
        // Multiple cycles make it more likely to cause visible corruption
    }
    
    std::cout << "All cycles completed without crash" << std::endl;
    SUCCEED();
}

TEST_F(MultiRingKTLSShutdownTest, LargeRingCount) {
    const std::string cert_path = "test_cert.pem";
    const std::string key_path = "test_key.pem";
    const int num_rings = 8;  // Larger ring count = more frees
    const int test_port = 8456;
    
    std::cout << "Creating server with " << num_rings << " rings (more frees to detect)..." << std::endl;
    
    {
        HttpServer server(num_rings, 128);
        server.addRoute("GET", "/", [](const HttpRequest&, HttpResponse& res) {
            res.setStatus(200);
        });
        
        if (!server.listenKTLS(test_port, cert_path, key_path)) {
            GTEST_SKIP() << "KTLS not available";
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server.stop();
        
        // With 8 rings, we have 9 frees of the same pointer!
    }
    
    SUCCEED();
}
