/// @file
/// Bounded lock-free single-producer, single-consumer queue
/// (`primitives.md` §3.3). Real-time safe on both ends once constructed.

#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <vector>

namespace primitives {

/// A ring buffer with one producer thread and one consumer thread.
/// @p T must be copy-assignable; slots are preallocated.
template <typename T>
class spscQueue {
public:
    /// Allocates room for at least @p capacity items.
    explicit spscQueue(std::size_t capacity)
        : slots_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)), mask_(slots_.size() - 1) {}

    spscQueue(const spscQueue&) = delete;
    spscQueue& operator=(const spscQueue&) = delete;

    /// Producer: appends @p item. Returns false when the queue is full.
    bool push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) == slots_.size()) return false;
        slots_[tail & mask_] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer: removes the oldest item into @p out. Returns false when
    /// the queue is empty.
    bool pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = slots_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Number of slots.
    std::size_t capacity() const noexcept { return slots_.size(); }

private:
    std::vector<T> slots_;
    std::size_t mask_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace primitives
