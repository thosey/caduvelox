#include <gtest/gtest.h>
#include "caduvelox/ServerConfig.hpp"
#include "caduvelox/Server.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>

using namespace caduvelox;

/**
 * Ring and buffer dimensions, and what a bad one reports (review items M5, M6).
 *
 * Measured on 7.2 before changing anything:
 *
 *   buffer_ring_count   powers of two from 1 to 32768 work; anything else fails
 *   queue_depth         1 to 32768 works (the kernel rounds up to a power of two)
 *   buffer_size_bytes   0 fails
 *
 * None of this was checked in ServerConfig::validate(). A bad buffer value was
 * found only when Server::init() tried to set the ring up, and every such failure
 * -- a count of 500, a count of 65536, a size of zero, an mmap that ran out of
 * memory -- threw the same message:
 *
 *   "Failed to setup buffer ring - this requires a recent kernel with buffer
 *    ring support"
 *
 * sending whoever hit it off to check their kernel version. The logged detail
 * was no better: "Unknown error -22", because a negative errno was passed to
 * strerror().
 */
namespace {

// Did validate() refuse, and does the refusal name the setting at fault?
void expectRejectedNaming(const ServerConfig& cfg, const std::string& setting) {
    try {
        cfg.validate();
        FAIL() << setting << " should have been rejected by validate()";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find(setting), std::string::npos)
            << "the error should say which setting is wrong; got: " << e.what();
    }
}

// ---------------------------------------------------------------------------
// ServerConfig::validate()
// ---------------------------------------------------------------------------

TEST(ServerConfigLimits, BufferRingCountMustBeAPowerOfTwo) {
    ServerConfig cfg;
    cfg.buffer_ring_count = 500;
    expectRejectedNaming(cfg, "buffer_ring_count");
}

TEST(ServerConfigLimits, BufferRingCountMustNotExceedTheKernelLimit) {
    for (unsigned count : {32769u, 65536u}) {
        ServerConfig cfg;
        cfg.buffer_ring_count = count;
        expectRejectedNaming(cfg, "buffer_ring_count");
    }
}

TEST(ServerConfigLimits, BufferRingCountMustNotBeZero) {
    ServerConfig cfg;
    cfg.buffer_ring_count = 0;
    expectRejectedNaming(cfg, "buffer_ring_count");
}

TEST(ServerConfigLimits, BufferSizeMustNotBeZero) {
    ServerConfig cfg;
    cfg.buffer_size_bytes = 0;
    expectRejectedNaming(cfg, "buffer_size_bytes");
}

TEST(ServerConfigLimits, QueueDepthMustBeWithinTheKernelRange) {
    for (unsigned depth : {0u, 32769u}) {
        ServerConfig cfg;
        cfg.queue_depth = depth;
        expectRejectedNaming(cfg, "queue_depth");
    }
}

// Guards: the boundaries that work must still be accepted.
TEST(ServerConfigLimits, TheValidBoundariesAreAccepted) {
    for (unsigned count : {1u, 2u, 512u, 32768u}) {
        ServerConfig cfg;
        cfg.buffer_ring_count = count;
        EXPECT_NO_THROW(cfg.validate()) << "buffer_ring_count=" << count;
    }
    for (unsigned depth : {1u, 300u, 32768u}) {
        ServerConfig cfg;
        cfg.queue_depth = depth;
        EXPECT_NO_THROW(cfg.validate())
            << "queue_depth=" << depth << " -- a non-power-of-two depth is fine, "
               "the kernel rounds it up";
    }
}

// ---------------------------------------------------------------------------
// Server::init(), which is public and does not go through ServerConfig
// ---------------------------------------------------------------------------

TEST(ServerInitLimits, ABadBufferCountSaysSoInsteadOfBlamingTheKernel) {
    static ConsoleLogger console_logger;
    Logger::setGlobalLogger(&console_logger);

    Server server;
    try {
        server.init(256, 500, 16384);
        FAIL() << "a buffer ring of 500 entries should not initialise";
    } catch (const std::exception& e) {
        const std::string what = e.what();
        EXPECT_EQ(what.find("kernel"), std::string::npos)
            << "500 is not a power of two; the kernel is not the problem. Got: " << what;
        EXPECT_NE(what.find("500"), std::string::npos)
            << "the error should name the value that was refused. Got: " << what;
    }
}

TEST(ServerInitLimits, AnAllocationFailureReportsTheRealReason) {
    static ConsoleLogger console_logger;
    Logger::setGlobalLogger(&console_logger);

    // The largest dimensions the limits allow: 2^15 buffers of 2^32 - 1 bytes is
    // about 2^47 bytes, the whole of a 47-bit user address space, so the mmap
    // fails with ENOMEM whatever the machine's memory or overcommit setting.
    Server server;
    try {
        server.init(256, 32768, UINT32_MAX);
        FAIL() << "a ~128 TiB buffer block should not be allocatable";
    } catch (const std::exception& e) {
        const std::string what = e.what();
        EXPECT_EQ(what.find("kernel"), std::string::npos)
            << "running out of memory is not a kernel-version problem. Got: " << what;
        EXPECT_NE(what.find("Cannot allocate memory"), std::string::npos)
            << "the error should carry the errno text. Got: " << what;
    }
}

/**
 * The kernel's per-buffer length is a u32, and the coordinator allocates
 * count * size bytes. With count 32768 and size 2^49 + 1 that product wraps
 * size_t to 32 KiB. Before the size was bounded, init() mmapped the 32 KiB,
 * registered 32768 buffers -- every one after the first pointing outside the
 * mapping, each with a length truncated to 1 -- and returned true. recv would
 * then have written network data to those addresses.
 *
 * Reachable only through Server::init() directly: ServerConfig's
 * buffer_size_bytes is an unsigned, which cannot hold a size this large.
 */
TEST(ServerInitLimits, ABufferSizeTheKernelCannotRepresentIsRefused) {
    static ConsoleLogger console_logger;
    Logger::setGlobalLogger(&console_logger);

    const size_t wrapping = (size_t(1) << 49) + 1;
    ASSERT_EQ(size_t(32768) * wrapping, size_t(32768))
        << "the arithmetic this test is about: the product wraps to 32 KiB";

    Server server;
    EXPECT_THROW(server.init(64, 32768, wrapping), std::invalid_argument)
        << "this used to initialise successfully with buffers outside their mapping";

    Server over;
    EXPECT_THROW(over.init(64, 8, size_t(UINT32_MAX) + 1), std::invalid_argument)
        << "a length the kernel's 32-bit field would truncate";
}

// ---------------------------------------------------------------------------
// M6: the constructor allocated a coordinator that init() threw away
// ---------------------------------------------------------------------------

TEST(ServerCoordinator, AnUninitialisedServerHasNoCoordinator) {
    Server server;
    EXPECT_EQ(server.getBufferRingCoordinator(), nullptr)
        << "the constructor built a BufferRingCoordinator that init() replaces "
           "unconditionally -- and until then it was a coordinator with no ring";
}

TEST(ServerCoordinator, AskingAnUninitialisedServerForItsGroupIdIsAnError) {
    Server server;
    EXPECT_THROW(server.getBufferGroupId(), std::logic_error)
        << "this used to answer 1, from a coordinator that was never set up";
}

// Guard: after init() everything is where it was.
TEST(ServerCoordinator, AnInitialisedServerHasItsCoordinator) {
    static ConsoleLogger console_logger;
    Logger::setGlobalLogger(&console_logger);

    Server server;
    ASSERT_TRUE(server.init(64, 8, 4096));
    ASSERT_NE(server.getBufferRingCoordinator(), nullptr);
    EXPECT_EQ(server.getBufferGroupId(), 1);
}

}  // namespace
