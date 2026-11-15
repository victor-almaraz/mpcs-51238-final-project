/// @file
/// Bounded lock-free single-producer, single-consumer queue
/// (`primitives.md` §3.3). Real-time safe on both ends once constructed.

#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace primitives {

/// A ring buffer with one producer thread and one consumer thread.
/// Slots are preallocated; items move in and out, so a queue of
/// `std::unique_ptr` hands ownership between threads without allocating.
template <typename T>
// head_ and tail_ each own a cache line, so the producer and consumer do
// not share one; the analyzer's tighter order would put them together.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): cache-line separation
class spscQueue {
public:
    /// Allocates room for at least @p capacity items.
    explicit spscQueue(std::size_t capacity)
        : slots_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)), mask_(slots_.size() - 1) {}

    spscQueue(const spscQueue&) = delete;
    spscQueue& operator=(const spscQueue&) = delete;

    /// Producer: appends a copy of @p item. Returns false when the queue
    /// is full.
    [[nodiscard]] bool push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        return put(item);
    }

    /// Producer: moves @p item in. Returns false when the queue is full, in
    /// which case @p item is untouched.
    [[nodiscard]] bool push(T&& item) noexcept { return put(std::move(item)); }

    /// Consumer: moves the oldest item into @p out. Returns false when the
    /// queue is empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = std::move(slots_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Consumer: true when there is nothing to pop.
    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_acquire);
    }

    /// Number of slots.
    std::size_t capacity() const noexcept { return slots_.size(); }

private:
    // Assigns the slot directly from @p item: one copy or one move.
    template <typename U>
    bool put(U&& item) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) == slots_.size()) return false;
        slots_[tail & mask_] = std::forward<U>(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::vector<T> slots_;
    const std::size_t mask_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace primitives
