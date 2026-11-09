// Lock-free single-producer / single-consumer ring buffer.
// Producers on non-audio threads may serialize themselves with a mutex; the
// audio thread (consumer) never blocks.
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace hsynth {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacityPow2 = 1024) { reset(capacityPow2); }

    // Not thread safe: call before use only.
    void reset(size_t capacityPow2) {
        size_t cap = 1;
        while (cap < capacityPow2) cap <<= 1;
        buffer_.assign(cap, T{});
        mask_ = cap - 1;
        head_.store(0);
        tail_.store(0);
    }

    bool push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= buffer_.size()) return false;  // full
        buffer_[tail & mask_] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) return false;
        out = buffer_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool full() const { return size() >= buffer_.size(); }
    size_t size() const { return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire); }

private:
    std::vector<T> buffer_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace hsynth
