#include <gtest/gtest.h>
#include "../../../src/ring_buffer/VyukovRingBuffer.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <set>
#include <algorithm>

// Test fixture for VyukovRingBuffer tests
class VyukovRingBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Any setup code here
    }

    void TearDown() override {
        // Any cleanup code here
    }
};

// Basic functionality tests
TEST_F(VyukovRingBufferTest, BasicEnqueueDequeue) {
    VyukovRingBuffer<int, 8> buffer;
    
    int value = 42;
    EXPECT_TRUE(buffer.enqueue(std::move(value)));
    
    int result;
    EXPECT_TRUE(buffer.dequeue(result));
    EXPECT_EQ(result, 42);
}

TEST_F(VyukovRingBufferTest, EnqueueByReference) {
    VyukovRingBuffer<int, 8> buffer;
    
    int value = 123;
    EXPECT_TRUE(buffer.enqueue(value)); // Copy version
    
    int result;
    EXPECT_TRUE(buffer.dequeue(result));
    EXPECT_EQ(result, 123);
}

TEST_F(VyukovRingBufferTest, EmptyBufferDequeue) {
    VyukovRingBuffer<int, 8> buffer;
    
    int result;
    EXPECT_FALSE(buffer.dequeue(result));
}

TEST_F(VyukovRingBufferTest, FullBufferEnqueue) {
    VyukovRingBuffer<int, 4> buffer; // Small buffer for easier testing
    
    // Fill the buffer completely
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(buffer.enqueue(i)) << "Failed to enqueue item " << i;
    }
    
    // Next enqueue should fail
    EXPECT_FALSE(buffer.enqueue(999));
}

TEST_F(VyukovRingBufferTest, FillAndDrainCycle) {
    VyukovRingBuffer<int, 8> buffer;
    
    // Fill buffer
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(buffer.enqueue(i));
    }
    
    // Drain buffer
    for (int i = 0; i < 8; ++i) {
        int result;
        EXPECT_TRUE(buffer.dequeue(result));
        EXPECT_EQ(result, i);
    }
    
    // Should be empty now
    int result;
    EXPECT_FALSE(buffer.dequeue(result));
}

TEST_F(VyukovRingBufferTest, MultipleEnqueueDequeueCycles) {
    VyukovRingBuffer<int, 8> buffer;
    
    for (int cycle = 0; cycle < 10; ++cycle) {
        // Fill buffer
        for (int i = 0; i < 8; ++i) {
            EXPECT_TRUE(buffer.enqueue(cycle * 100 + i));
        }
        
        // Drain buffer
        for (int i = 0; i < 8; ++i) {
            int result;
            EXPECT_TRUE(buffer.dequeue(result));
            EXPECT_EQ(result, cycle * 100 + i);
        }
    }
}

TEST_F(VyukovRingBufferTest, PartialFillDrain) {
    VyukovRingBuffer<int, 8> buffer;
    
    // Add 3 items
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(buffer.enqueue(i));
    }
    
    // Remove 2 items
    for (int i = 0; i < 2; ++i) {
        int result;
        EXPECT_TRUE(buffer.dequeue(result));
        EXPECT_EQ(result, i);
    }
    
    // Add 5 more items (should fit: 1 existing + 5 new = 6 total)
    for (int i = 10; i < 15; ++i) {
        EXPECT_TRUE(buffer.enqueue(i));
    }
    
    // Should now have: [2, 10, 11, 12, 13, 14]
    std::vector<int> expected = {2, 10, 11, 12, 13, 14};
    for (int expected_val : expected) {
        int result;
        EXPECT_TRUE(buffer.dequeue(result));
        EXPECT_EQ(result, expected_val);
    }
}

// String tests to verify move semantics work properly
TEST_F(VyukovRingBufferTest, StringMoveSemantics) {
    VyukovRingBuffer<std::string, 4> buffer;
    
    std::string test_str = "Hello, World!";
    std::string original = test_str;
    
    EXPECT_TRUE(buffer.enqueue(std::move(test_str)));
    // test_str should now be empty or in moved-from state
    
    std::string result;
    EXPECT_TRUE(buffer.dequeue(result));
    EXPECT_EQ(result, original);
}

// Stress tests for single-threaded scenarios
TEST_F(VyukovRingBufferTest, LargeNumberOfOperations) {
    VyukovRingBuffer<int, 64> buffer;
    const int num_operations = 10000;
    
    for (int i = 0; i < num_operations; ++i) {
        // Fill buffer partially
        for (int j = 0; j < 32; ++j) {
            EXPECT_TRUE(buffer.enqueue(i * 1000 + j));
        }
        
        // Drain buffer partially
        for (int j = 0; j < 32; ++j) {
            int result;
            EXPECT_TRUE(buffer.dequeue(result));
            EXPECT_EQ(result, i * 1000 + j);
        }
    }
}

// Multi-threaded tests
TEST_F(VyukovRingBufferTest, SingleProducerSingleConsumer) {
    VyukovRingBuffer<int, 64> buffer;
    const int num_items = 10000;
    std::atomic<bool> producer_done{false};
    std::vector<int> consumed_items;
    consumed_items.reserve(num_items);
    
    // Producer thread
    std::thread producer([&buffer, num_items, &producer_done]() {
        for (int i = 0; i < num_items; ++i) {
            while (!buffer.enqueue(i)) {
                std::this_thread::yield();
            }
        }
        producer_done = true;
    });
    
    // Consumer thread
    std::thread consumer([&buffer, &consumed_items, &producer_done]() {
        int item;
        while (true) {
            if (buffer.dequeue(item)) {
                consumed_items.push_back(item);
            } else if (producer_done.load()) {
                // Producer is done and buffer is empty, final check
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });
    
    producer.join();
    consumer.join();
    
    // Verify all items were consumed in order
    EXPECT_EQ(consumed_items.size(), num_items);
    if (consumed_items.size() == num_items) {
        for (int i = 0; i < num_items; ++i) {
            EXPECT_EQ(consumed_items[i], i);
        }
    }
}

TEST_F(VyukovRingBufferTest, MultipleProducersMultipleConsumers) {
    VyukovRingBuffer<int, 128> buffer;
    const int num_producers = 4;
    const int num_consumers = 4;
    const int items_per_producer = 1000;
    const int total_items = num_producers * items_per_producer;
    
    std::atomic<int> producers_done{0};
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    std::atomic<int> total_consumed{0};
    std::vector<std::set<int>> consumed_by_thread(num_consumers);
    
    // Start producer threads
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&buffer, p, items_per_producer, &producers_done]() {
            for (int i = 0; i < items_per_producer; ++i) {
                int item = p * 10000 + i; // Unique item per producer
                while (!buffer.enqueue(item)) {
                    std::this_thread::yield();
                }
            }
            producers_done++;
        });
    }
    
    // Start consumer threads
    for (int c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&buffer, c, &consumed_by_thread, &total_consumed, 
                               &producers_done, num_producers]() {
            int item;
            while (true) {
                if (buffer.dequeue(item)) {
                    consumed_by_thread[c].insert(item);
                    total_consumed++;
                } else if (producers_done.load() >= num_producers) {
                    // All producers done and buffer is empty
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    
    // Wait for all threads
    for (auto& p : producers) p.join();
    for (auto& c : consumers) c.join();
    
    // Verify total consumption
    EXPECT_EQ(total_consumed.load(), total_items);
    
    // Verify no duplicates across all consumers
    std::set<int> all_consumed;
    for (const auto& consumer_set : consumed_by_thread) {
        for (int item : consumer_set) {
            EXPECT_TRUE(all_consumed.insert(item).second) << "Duplicate item found: " << item;
        }
    }
    
    EXPECT_EQ(all_consumed.size(), total_items);
}

TEST_F(VyukovRingBufferTest, HighContentionScenario) {
    VyukovRingBuffer<int, 32> buffer; // Smaller buffer for more contention
    const int num_threads = 8;
    const int operations_per_thread = 1000;
    std::atomic<int> total_enqueued{0};
    std::atomic<int> total_dequeued{0};
    std::vector<std::thread> threads;
    
    // Each thread both produces and consumes
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&buffer, t, operations_per_thread, 
                             &total_enqueued, &total_dequeued]() {
            std::mt19937 rng(t); // Thread-local RNG
            std::uniform_int_distribution<int> dist(0, 1);
            
            int produced = 0;
            int consumed = 0;
            
            for (int op = 0; op < operations_per_thread * 2; ++op) {
                if (dist(rng) == 0 && produced < operations_per_thread) {
                    // Try to produce
                    int item = t * 10000 + produced;
                    if (buffer.enqueue(item)) {
                        produced++;
                        total_enqueued++;
                    }
                } else {
                    // Try to consume
                    int item;
                    if (buffer.dequeue(item)) {
                        consumed++;
                        total_dequeued++;
                    }
                }
                
                if (produced >= operations_per_thread && consumed >= operations_per_thread) {
                    break;
                }
            }
        });
    }
    
    for (auto& t : threads) t.join();
    
    // In this test, we don't expect perfect balance due to the random nature,
    // but we should have reasonable production and consumption
    EXPECT_GT(total_enqueued.load(), 0);
    EXPECT_GT(total_dequeued.load(), 0);
    
    // The difference should be reasonable (items left in buffer)
    int difference = total_enqueued - total_dequeued;
    EXPECT_GE(difference, 0);
    EXPECT_LE(difference, 32); // At most buffer size items left
}

// Performance benchmark (not a real test, but useful for development)
TEST_F(VyukovRingBufferTest, PerformanceBenchmark) {
    VyukovRingBuffer<int, 1024> buffer;
    const int num_operations = 1000000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Single-threaded throughput test
    for (int i = 0; i < num_operations; ++i) {
        while (!buffer.enqueue(i)) {
            int dummy;
            buffer.dequeue(dummy); // Make space
        }
        
        if (i % 2 == 1) { // Dequeue every other operation
            int result;
            buffer.dequeue(result);
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // This is informational - not a real assertion
    std::cout << "Performance: " << num_operations 
              << " operations in " << duration.count() 
              << " microseconds (" 
              << (num_operations * 1000000.0 / duration.count()) 
              << " ops/sec)" << std::endl;
    
    // Just verify we can still operate
    EXPECT_TRUE(buffer.enqueue(999));
}

// Edge case tests
TEST_F(VyukovRingBufferTest, WrapAroundBehavior) {
    VyukovRingBuffer<size_t, 4> buffer;
    
    // Fill and drain multiple times to test wraparound
    for (size_t cycle = 0; cycle < 1000; ++cycle) {
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_TRUE(buffer.enqueue(cycle * 4 + i));
        }
        
        for (size_t i = 0; i < 4; ++i) {
            size_t result;
            EXPECT_TRUE(buffer.dequeue(result));
            EXPECT_EQ(result, cycle * 4 + i);
        }
    }
}

// Test with complex types
struct ComplexType {
    int id;
    std::string data;
    std::vector<int> numbers;
    
    ComplexType() : id(0), data(""), numbers{} {} // Default constructor
    
    ComplexType(int i, const std::string& d) : id(i), data(d) {
        numbers = {i, i+1, i+2};
    }
    
    bool operator==(const ComplexType& other) const {
        return id == other.id && data == other.data && numbers == other.numbers;
    }
};

TEST_F(VyukovRingBufferTest, ComplexTypeHandling) {
    VyukovRingBuffer<ComplexType, 8> buffer;
    
    ComplexType obj1(1, "first");
    ComplexType obj2(2, "second");
    
    EXPECT_TRUE(buffer.enqueue(std::move(obj1)));
    EXPECT_TRUE(buffer.enqueue(std::move(obj2)));
    
    ComplexType result1(0, "");
    ComplexType result2(0, "");
    
    EXPECT_TRUE(buffer.dequeue(result1));
    EXPECT_TRUE(buffer.dequeue(result2));
    
    EXPECT_EQ(result1.id, 1);
    EXPECT_EQ(result1.data, "first");
    EXPECT_EQ(result2.id, 2);
    EXPECT_EQ(result2.data, "second");
}
