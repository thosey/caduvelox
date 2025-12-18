#include <gtest/gtest.h>
#include "../../../src/ring_buffer/NotifyingRingBuffer.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>

// Test fixture for NotifyingRingBuffer tests
class NotifyingRingBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Any setup code here
    }

    void TearDown() override {
        // Any cleanup code here
    }
};

// Basic functionality tests
TEST_F(NotifyingRingBufferTest, BasicEnqueueDequeue) {
    NotifyingRingBuffer<int, 8> buffer;
    
    int value = 42;
    EXPECT_TRUE(buffer.enqueue(std::move(value)));
    
    int result;
    EXPECT_TRUE(buffer.dequeue(result));
    EXPECT_EQ(result, 42);
}

TEST_F(NotifyingRingBufferTest, EnqueueByReference) {
    NotifyingRingBuffer<int, 8> buffer;
    
    int value = 123;
    EXPECT_TRUE(buffer.enqueue(value)); // Copy version
    
    int result;
    EXPECT_TRUE(buffer.dequeue(result));
    EXPECT_EQ(result, 123);
}

TEST_F(NotifyingRingBufferTest, TryDequeueNonBlocking) {
    NotifyingRingBuffer<int, 8> buffer;
    
    // Should return false immediately when empty
    int result;
    EXPECT_FALSE(buffer.try_dequeue(result));
    
    // Add item and try again
    EXPECT_TRUE(buffer.enqueue(99));
    EXPECT_TRUE(buffer.try_dequeue(result));
    EXPECT_EQ(result, 99);
}

TEST_F(NotifyingRingBufferTest, FullBufferEnqueue) {
    NotifyingRingBuffer<int, 4> buffer;
    
    // Fill the buffer completely
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(buffer.enqueue(i)) << "Failed to enqueue item " << i;
    }
    
    // Next enqueue should fail
    EXPECT_FALSE(buffer.enqueue(999));
}

// Test blocking behavior
TEST_F(NotifyingRingBufferTest, BlockingDequeue) {
    NotifyingRingBuffer<int, 8> buffer;
    std::atomic<bool> consumer_started{false};
    std::atomic<bool> consumer_finished{false};
    int result = -1;
    
    // Start consumer thread that will block
    std::thread consumer([&buffer, &consumer_started, &consumer_finished, &result]() {
        consumer_started = true;
        buffer.dequeue(result);  // This should block
        consumer_finished = true;
    });
    
    // Wait for consumer to start
    while (!consumer_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    // Give consumer time to block
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(consumer_finished.load());
    
    // Produce an item - should wake the consumer
    EXPECT_TRUE(buffer.enqueue(42));
    
    consumer.join();
    EXPECT_TRUE(consumer_finished.load());
    EXPECT_EQ(result, 42);
}

// Test producer-consumer scenario
TEST_F(NotifyingRingBufferTest, SingleProducerSingleConsumerBlocking) {
    NotifyingRingBuffer<int, 64> buffer;
    const int num_items = 1000;
    std::vector<int> consumed_items;
    consumed_items.reserve(num_items);
    std::atomic<bool> producer_done{false};
    
    // Producer thread
    std::thread producer([&buffer, num_items, &producer_done]() {
        for (int i = 0; i < num_items; ++i) {
            while (!buffer.enqueue(i)) {
                std::this_thread::yield(); // Buffer full, retry
            }
            
            // Add some delay to test blocking behavior
            if (i % 100 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        producer_done = true;
        buffer.shutdown(); // Signal shutdown to consumer
    });
    
    // Consumer thread using blocking dequeue
    std::thread consumer([&buffer, &consumed_items, &producer_done]() {
        int item;
        while (true) {
            if (buffer.dequeue(item)) {
                consumed_items.push_back(item);
            } else {
                // dequeue returned false, check if producer is done
                break;
            }
        }
        
        // Drain any remaining items with non-blocking calls
        while (buffer.try_dequeue(item)) {
            consumed_items.push_back(item);
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

// Test multiple producers, single consumer
TEST_F(NotifyingRingBufferTest, MultipleProducersSingleConsumer) {
    NotifyingRingBuffer<int, 128> buffer;
    const int num_producers = 4;
    const int items_per_producer = 250;
    const int total_items = num_producers * items_per_producer;
    
    std::atomic<int> producers_done{0};
    std::vector<std::thread> producers;
    std::vector<int> consumed_items;
    consumed_items.reserve(total_items);
    
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
    
    // Consumer thread using blocking dequeue
    std::thread consumer([&buffer, &consumed_items, &producers_done, num_producers]() {
        int item;
        while (true) {
            if (buffer.dequeue(item)) {
                consumed_items.push_back(item);
            } else {
                // dequeue returned false (shutdown), drain remaining items
                break;
            }
        }
        
        // Final drain with non-blocking calls
        while (buffer.try_dequeue(item)) {
            consumed_items.push_back(item);
        }
    });
    
    // Wait for all producers to finish
    for (auto& p : producers) p.join();
    
    // Signal shutdown to consumer
    buffer.shutdown();
    consumer.join();
    
    // Verify total consumption
    EXPECT_EQ(consumed_items.size(), total_items);
    
    // Verify no duplicates
    std::sort(consumed_items.begin(), consumed_items.end());
    auto last = std::unique(consumed_items.begin(), consumed_items.end());
    EXPECT_EQ(std::distance(consumed_items.begin(), last), total_items);
}

// Performance comparison test
TEST_F(NotifyingRingBufferTest, PerformanceComparison) {
    NotifyingRingBuffer<int, 1024> buffer;
    const int num_operations = 100000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Producer-consumer with blocking
    std::thread producer([&buffer, num_operations]() {
        for (int i = 0; i < num_operations; ++i) {
            while (!buffer.enqueue(i)) {
                std::this_thread::yield();
            }
        }
    });
    
    std::thread consumer([&buffer, num_operations]() {
        int item;
        for (int i = 0; i < num_operations; ++i) {
            while (!buffer.dequeue(item)) {
                // This would busy-wait in the old implementation
                // but now blocks efficiently
            }
        }
    });
    
    producer.join();
    consumer.join();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "NotifyingRingBuffer Performance (with blocking): " << num_operations 
              << " operations in " << duration.count() 
              << " microseconds (" 
              << (num_operations * 1000000.0 / duration.count()) 
              << " ops/sec)" << std::endl;
    
    // Should still be very fast
    EXPECT_LT(duration.count(), 1000000); // Less than 1 second for 100k ops
}

// Test atomic overhead without blocking
TEST_F(NotifyingRingBufferTest, AtomicOverheadBenchmark) {
    NotifyingRingBuffer<int, 1024> buffer;
    const int num_operations = 1000000;
    
    // Pre-fill buffer to avoid blocking (leave one slot free)
    for (int i = 0; i < 1023; ++i) {
        ASSERT_TRUE(buffer.enqueue(i));
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Single-threaded throughput test - no blocking should occur
    for (int i = 0; i < num_operations; ++i) {
        int item;
        ASSERT_TRUE(buffer.dequeue(item)); // Should never block - buffer has data
        ASSERT_TRUE(buffer.enqueue(item + 10000)); // Should never block - space available
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "NotifyingRingBuffer Atomic Overhead (no blocking): " << num_operations 
              << " operations in " << duration.count() 
              << " microseconds (" 
              << (num_operations * 1000000.0 / duration.count()) 
              << " ops/sec)" << std::endl;
    
    // Compare with raw MPMCRingBuffer performance (~29M ops/sec)
    // This should be much closer to the raw performance
}

// Direct comparison with raw MPMCRingBuffer
TEST_F(NotifyingRingBufferTest, RawVsNotifyingComparison) {
    const int num_operations = 1000000;
    
    // Test raw MPMCRingBuffer
    {
        MPMCRingBuffer<int, 1024> raw_buffer;
        
        // Pre-fill buffer (leave one slot free)
        for (int i = 0; i < 1023; ++i) {
            ASSERT_TRUE(raw_buffer.enqueue(i));
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_operations; ++i) {
            int item;
            ASSERT_TRUE(raw_buffer.dequeue(item));
            ASSERT_TRUE(raw_buffer.enqueue(item + 10000));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "Raw MPMCRingBuffer: " << num_operations 
                  << " operations in " << duration.count() 
                  << " microseconds (" 
                  << (num_operations * 1000000.0 / duration.count()) 
                  << " ops/sec)" << std::endl;
    }
    
    // Test NotifyingRingBuffer (no blocking scenario)
    {
        NotifyingRingBuffer<int, 1024> notifying_buffer;
        
        // Pre-fill buffer (leave one slot free)
        for (int i = 0; i < 1023; ++i) {
            ASSERT_TRUE(notifying_buffer.enqueue(i));
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_operations; ++i) {
            int item;
            ASSERT_TRUE(notifying_buffer.dequeue(item));
            ASSERT_TRUE(notifying_buffer.enqueue(item + 10000));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "NotifyingRingBuffer (no blocking): " << num_operations 
                  << " operations in " << duration.count() 
                  << " microseconds (" 
                  << (num_operations * 1000000.0 / duration.count()) 
                  << " ops/sec)" << std::endl;
    }
}

// Test notification behavior
TEST_F(NotifyingRingBufferTest, NotifyAllWakesMultipleConsumers) {
    NotifyingRingBuffer<int, 8> buffer;
    const int num_consumers = 3;
    std::atomic<int> consumers_woken{0};
    std::vector<std::thread> consumers;
    
    // Start multiple consumers that will block
    for (int i = 0; i < num_consumers; ++i) {
        consumers.emplace_back([&buffer, &consumers_woken]() {
            int item;
            buffer.dequeue(item); // Will block since buffer is empty
            consumers_woken++;
        });
    }
    
    // Give consumers time to start and block
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Add items for consumers
    for (int i = 0; i < num_consumers; ++i) {
        EXPECT_TRUE(buffer.enqueue(i));
    }
    
    // Notify all consumers
    buffer.notify_all();
    
    // Wait for all consumers
    for (auto& c : consumers) {
        c.join();
    }
    
    EXPECT_EQ(consumers_woken.load(), num_consumers);
}

// Test with complex types
TEST_F(NotifyingRingBufferTest, ComplexTypeHandling) {
    struct ComplexType {
        int id;
        std::string data;
        std::vector<int> numbers;
        
        ComplexType() : id(0), data(""), numbers{} {}
        
        ComplexType(int i, const std::string& d) : id(i), data(d) {
            numbers = {i, i+1, i+2};
        }
        
        bool operator==(const ComplexType& other) const {
            return id == other.id && data == other.data && numbers == other.numbers;
        }
    };
    
    NotifyingRingBuffer<ComplexType, 8> buffer;
    
    ComplexType obj1(1, "first");
    ComplexType obj2(2, "second");
    
    EXPECT_TRUE(buffer.enqueue(std::move(obj1)));
    EXPECT_TRUE(buffer.enqueue(std::move(obj2)));
    
    ComplexType result1;
    ComplexType result2;
    
    EXPECT_TRUE(buffer.dequeue(result1));
    EXPECT_TRUE(buffer.dequeue(result2));
    
    EXPECT_EQ(result1.id, 1);
    EXPECT_EQ(result1.data, "first");
    EXPECT_EQ(result2.id, 2);
    EXPECT_EQ(result2.data, "second");
}
