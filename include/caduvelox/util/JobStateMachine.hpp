#pragma once

#include <atomic>

namespace caduvelox {

/**
 * Lock-free state machine for managing job lifetime across async operations
 * 
 * This class provides atomic state transitions to prevent use-after-free when
 * a job can be accessed by multiple threads (e.g., io_uring thread and worker threads).
 * 
 * State Transitions:
 * - AVAILABLE -> WORKING: Worker acquires job for processing (tryAcquire)
 * - WORKING -> AVAILABLE: Worker finishes processing normally (tryRelease)
 * - AVAILABLE -> DISCARDED: Connection closes while idle (tryDiscard)
 * - WORKING -> DISCARDED: Connection closes while worker processing (tryDiscard)
 * 
 * Cleanup Protocol:
 * - If tryDiscard succeeds (AVAILABLE->DISCARDED), caller must cleanup
 * - If tryRelease fails (WORKING->DISCARDED), worker must cleanup
 * - This ensures exactly one thread performs cleanup
 * 
 * Thread Safety:
 * All methods use atomic compare-exchange with appropriate memory ordering.
 * Safe to call from multiple threads concurrently.
 * 
 * Example Usage:
 * ```cpp
 * class MyJob {
 *     JobStateMachine state_;
 * 
 *     void workerMethod() {
 *         if (!state_.tryAcquire()) return;  // Job was discarded
 *         
 *         // Do work...
 *         
 *         if (!state_.tryRelease()) {
 *             // Job was discarded while we worked - cleanup!
 *             returnToPool(this);
 *         }
 *     }
 * 
 *     void close() {
 *         if (state_.tryDiscard()) {
 *             // We must cleanup
 *             returnToPool(this);
 *         }
 *         // else: Worker will cleanup
 *     }
 * };
 * ```
 */
class JobStateMachine {
public:
    enum class State : int {
        AVAILABLE = 0,   // Ready for worker processing
        WORKING = 1,     // Currently being processed by worker
        DISCARDED = 2    // Marked for cleanup, pending final release
    };

    /**
     * Initialize state machine in AVAILABLE state
     */
    JobStateMachine() : state_(State::AVAILABLE) {}

    /**
     * Try to acquire job for worker processing
     * Attempts transition: AVAILABLE -> WORKING
     * 
     * @return true if successfully acquired, false if job is discarded
     */
    bool tryAcquire() {
        State expected = State::AVAILABLE;
        return state_.compare_exchange_strong(
            expected, 
            State::WORKING,
            std::memory_order_acquire,  // Synchronize with previous release
            std::memory_order_relaxed
        );
    }

    /**
     * Release job after worker processing
     * Attempts transition: WORKING -> AVAILABLE
     * 
     * @return true if released normally, false if job was discarded (caller must cleanup!)
     */
    bool tryRelease() {
        State expected = State::WORKING;
        if (state_.compare_exchange_strong(
                expected,
                State::AVAILABLE,
                std::memory_order_release,  // Make all work visible to next acquire
                std::memory_order_relaxed)) {
            return true;  // Normal release
        }
        
        // CAS failed - state must be DISCARDED now
        // We're the last thread with access, so we must cleanup
        return false;
    }

    /**
     * Mark job as discarded (connection closed)
     * Attempts transitions:
     * - AVAILABLE -> DISCARDED (succeeds, caller must cleanup)
     * - WORKING -> DISCARDED (succeeds, worker will cleanup)
     * 
     * @return true if caller should cleanup immediately, false if worker will cleanup
     */
    bool tryDiscard() {
        State expected = State::AVAILABLE;
        if (state_.compare_exchange_strong(
                expected,
                State::DISCARDED,
                std::memory_order_acq_rel,  // Full fence for coordination
                std::memory_order_relaxed)) {
            return true;  // Successfully discarded idle job - caller must cleanup
        }
        
        // Job is WORKING - mark as discarded so worker will cleanup when done
        expected = State::WORKING;
        state_.compare_exchange_strong(
            expected,
            State::DISCARDED,
            std::memory_order_acq_rel,
            std::memory_order_relaxed
        );
        
        // Whether CAS succeeded or failed, worker will cleanup
        // (if it failed, job was already DISCARDED by another thread)
        return false;
    }

    /**
     * Get current state (for testing/debugging only)
     * Note: State may change immediately after reading
     */
    State getState() const {
        return state_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<State> state_;
};

} // namespace caduvelox
