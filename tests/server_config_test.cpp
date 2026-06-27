#include <gtest/gtest.h>
#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/http/HttpServer.hpp"
#include "caduvelox/jobs/KTLSJob.hpp"
#include "caduvelox/jobs/AcceptJob.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/util/PoolManager.hpp"
#include <stdexcept>

namespace {

// ============================================================================
// ServerConfig default values
// ============================================================================

TEST(ServerConfigTest, DefaultsMatchHardCodedValues) {
    caduvelox::ServerConfig cfg;
    EXPECT_EQ(cfg.queue_depth,               4096u);
    EXPECT_EQ(cfg.buffer_ring_count,          512u);
    EXPECT_EQ(cfg.buffer_size_bytes,        16384u);
    EXPECT_EQ(cfg.ktls_pool_size,            5000u);
    EXPECT_EQ(cfg.accept_pool_size,          1000u);
    EXPECT_EQ(cfg.write_pool_size,          10000u);
    EXPECT_EQ(cfg.file_job_pool_size,        1000u);
    EXPECT_EQ(cfg.connection_pool_size,     10000u);
    EXPECT_EQ(cfg.ktls_handshake_timeout_ms, 5000u);
    EXPECT_EQ(cfg.num_rings,                    0);
}

TEST(ServerConfigTest, FieldsAreIndependentlySettable) {
    caduvelox::ServerConfig cfg;
    cfg.queue_depth    = 2048;
    cfg.num_rings      = 4;
    cfg.ktls_pool_size = 128;

    EXPECT_EQ(cfg.queue_depth,    2048u);
    EXPECT_EQ(cfg.num_rings,          4);
    EXPECT_EQ(cfg.ktls_pool_size,  128u);
    // Others unchanged
    EXPECT_EQ(cfg.buffer_ring_count, 512u);
}

// ============================================================================
// HttpServer — ServerConfig constructor
// ============================================================================

TEST(ServerConfigTest, HttpServerDefaultConstructorUsesDefaults) {
    // Default construction must not throw and must set num_rings > 0.
    caduvelox::HttpServer server;
    EXPECT_GT(server.getConfig().num_rings, 0);
    EXPECT_EQ(server.getConfig().queue_depth, 4096u);
}

TEST(ServerConfigTest, HttpServerResolvesZeroNumRings) {
    caduvelox::ServerConfig cfg;
    cfg.num_rings = 0;
    caduvelox::HttpServer server(cfg);
    // After construction, num_rings must have been resolved to hardware_concurrency.
    EXPECT_GT(server.getConfig().num_rings, 0);
}

TEST(ServerConfigTest, HttpServerStoresExplicitNumRings) {
    caduvelox::ServerConfig cfg;
    cfg.num_rings = 2;
    caduvelox::HttpServer server(cfg);
    EXPECT_EQ(server.getConfig().num_rings, 2);
}

TEST(ServerConfigTest, HttpServerConvenienceConstructorPreservesQueueDepth) {
    caduvelox::HttpServer server(1, 2048);
    EXPECT_EQ(server.getConfig().queue_depth, 2048u);
    EXPECT_EQ(server.getConfig().num_rings,   1);
}

// ============================================================================
// PoolManager::setCapacity / getCapacity round-trip
// ============================================================================

TEST(ServerConfigTest, SetCapacityIsObservableViaGetCapacity) {
    const size_t saved = caduvelox::PoolManager::getCapacity<caduvelox::KTLSJob>();

    caduvelox::PoolManager::setCapacity<caduvelox::KTLSJob>(42u);
    EXPECT_EQ(caduvelox::PoolManager::getCapacity<caduvelox::KTLSJob>(), 42u);

    // Restore so other tests are unaffected.
    caduvelox::PoolManager::setCapacity<caduvelox::KTLSJob>(saved);
    EXPECT_EQ(caduvelox::PoolManager::getCapacity<caduvelox::KTLSJob>(), saved);
}

TEST(ServerConfigTest, DefaultPoolCapacitiesMatchServerConfigDefaults) {
    caduvelox::ServerConfig defaults;
    // KTLSJob and AcceptJob default capacities must match ServerConfig defaults
    // (verified via the PoolCapacityConfig specializations in their .cpp files).
    EXPECT_EQ(caduvelox::PoolManager::getCapacity<caduvelox::KTLSJob>(),
              static_cast<size_t>(defaults.ktls_pool_size));
    EXPECT_EQ(caduvelox::PoolManager::getCapacity<caduvelox::AcceptJob>(),
              static_cast<size_t>(defaults.accept_pool_size));
    EXPECT_EQ(caduvelox::PoolManager::getCapacity<caduvelox::WriteJob>(),
              static_cast<size_t>(defaults.write_pool_size));
}

// ============================================================================
// Pool capacity validation — item #16
// ============================================================================

TEST(ServerConfigTest, ValidatePassesForDefaultConfig) {
    caduvelox::ServerConfig cfg;
    EXPECT_NO_THROW(cfg.validate());
}

TEST(ServerConfigTest, ValidateThrowsOnZeroConnectionPool) {
    caduvelox::ServerConfig cfg;
    cfg.connection_pool_size = 0;
    EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(ServerConfigTest, ValidateThrowsOnZeroKtlsPool) {
    caduvelox::ServerConfig cfg;
    cfg.ktls_pool_size = 0;
    EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(ServerConfigTest, HttpServerThrowsOnZeroConnectionPoolSize) {
    caduvelox::ServerConfig cfg;
    cfg.connection_pool_size = 0;
    EXPECT_THROW(caduvelox::HttpServer server(cfg), std::invalid_argument);
}

TEST(ServerConfigTest, SetCapacityZeroAssertsInDebug) {
    // In debug builds (NDEBUG not defined), setCapacity(0) triggers an assert.
    // In release builds this check is skipped; ServerConfig::validate() is the
    // production-time guard for misconfigured capacities.
    EXPECT_DEBUG_DEATH(
        caduvelox::PoolManager::setCapacity<caduvelox::KTLSJob>(0u),
        "cap > 0");
}

} // namespace
