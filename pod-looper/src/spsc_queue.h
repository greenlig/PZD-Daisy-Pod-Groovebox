// Single-producer / single-consumer lock-free queue.
//
// Used to hand MIDI from the UART interrupt and the main loop to the audio
// callback without locks. Exactly one context may Push and one may Pop.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

template <typename T, size_t N>
class SpscQueue
{
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");

  public:
    bool Push(const T& v)
    {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if(head - tail >= N)
            return false; // full: drop it
        buf_[head & (N - 1)] = v;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool Pop(T& v)
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if(head == tail)
            return false;
        v = buf_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

  private:
    T                     buf_[N];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
};
