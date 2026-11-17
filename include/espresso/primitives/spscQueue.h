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

#include <espresso/primitives/realtime.h>

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

    ~spscQueue() = default;
    spscQueue(const spscQueue&) = delete;
    spscQueue& operator=(const spscQueue&) = delete;
    spscQueue(spscQueue&&) = delete;
    spscQueue& operator=(spscQueue&&) = delete;

    /// Producer: appends a copy of @p item. Returns false when the queue
    /// is full.
    [[nodiscard]] bool
    push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) ESPRESSO_NONBLOCKING {
        return put(item);
    }

    /// Producer: moves @p item in. Returns false when the queue is full, in
    /// which case @p item is untouched.
    [[nodiscard]] bool push(T&& item) noexcept ESPRESSO_NONBLOCKING { return put(std::move(item)); }

    /// Consumer: moves the oldest item into @p out, which holds nothing to
    /// free (an empty or moved-from owner). Returns false when the queue
    /// is empty.
    [[nodiscard]] bool pop(T& out) noexcept ESPRESSO_NONBLOCKING {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // out holds nothing to free (above)
        out = std::move(slots_[head & mask_]);
        ESPRESSO_ASSUME_NONBLOCKING_END
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Consumer: true when there is nothing to pop.
    bool empty() const noexcept ESPRESSO_NONBLOCKING {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_acquire);
    }

    /// Number of slots.
    std::size_t capacity() const noexcept ESPRESSO_NONBLOCKING { return slots_.size(); }

private:
    // Assigns the slot directly from @p item: one copy or one move.
    template <typename U>
    bool put(U&& item) ESPRESSO_NONBLOCKING {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) == slots_.size()) return false;
        // The slot holds nothing to free: empty since construction, or moved
        // from by the pop that last took its item.
        ESPRESSO_ASSUME_NONBLOCKING_BEGIN
        slots_[tail & mask_] = std::forward<U>(item);
        ESPRESSO_ASSUME_NONBLOCKING_END
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::vector<T> slots_;
    const std::size_t mask_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace primitives
