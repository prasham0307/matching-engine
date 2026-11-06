#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace lob {

inline constexpr std::size_t kCacheLineBytes = 64;

// Bounded single-producer single-consumer queue. Exactly one thread may call the push side
// (try_push) and exactly one thread may call the pop side (try_pop, try_pop_batch), for the whole
// life of the queue. Nothing here locks, and nothing allocates after the constructor.
//
// Layout and protocol
// -------------------
// head_ counts every item ever popped, tail_ every item ever pushed. They only increase and are
// never reduced modulo the capacity (a 64-bit counter will not wrap in practice), and the slot
// for counter c is c & mask_. Empty is head == tail, full is tail - head == capacity, so all
// `capacity` slots are usable and there is no ambiguity between the two cases.
//
// Cached = true: each side keeps a private copy of the other side's counter and reloads the real
// one (an acquire load, a cache miss when the other thread has been writing it) only when the
// copy says the queue is full or empty. The copy is always safe to use: the real producer counter
// only grows, and so does the real consumer counter, so a stale copy can only make the queue look
// fuller (for the producer) or emptier (for the consumer) than it is, never the other way round.
// Cached = false reloads every time, which exists to measure what the cache saves.
//
// Padded = true: the producer's counter and its private copy share one 64-byte line, the
// consumer's counter and its copy share another, and the read-only fields get a third. Each line
// then has one writer. Padded = false packs everything into one line, so every store by one
// thread invalidates the other thread's copy of the line (false sharing). It exists to measure
// that, not to be used.
//
// What is NOT padded: the slots. When the queue is nearly empty or full, the producer writes a
// slot in the same cache line the consumer is reading. Padding each slot to a line would waste
// memory, so that sharing stays.
//
// T must be default constructible and copy assignable; slots are default constructed up front and
// assigned to on push and pop.
template <class T, bool Cached = true, bool Padded = true>
class alignas(kCacheLineBytes) SpscQueue {
  static_assert(std::is_default_constructible_v<T>, "slots are default constructed");
  static_assert(std::is_copy_assignable_v<T>, "items are copied in and out");

 public:
  // `capacity` must be a power of two (so the slot index is a mask, not a division). Throws
  // std::invalid_argument otherwise. This is the only allocation the queue ever makes.
  explicit SpscQueue(std::size_t capacity)
      : capacity_(checked_capacity(capacity)),
        mask_(capacity - 1),
        slots_(std::make_unique<T[]>(capacity)) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  std::size_t capacity() const { return capacity_; }

  // Producer only. Returns false, leaving the queue unchanged, if it is full.
  bool try_push(const T& value) {
    // Relaxed: only this thread ever writes tail_, so it can read its own last value without
    // any ordering.
    const uint64_t tail = tail_.load(std::memory_order_relaxed);

    if constexpr (Cached) {
      if (tail - cached_head_ >= capacity_) {  // full by the stale copy: look at the real head
        // Acquire: pairs with the consumer's release store of head_. It makes the consumer's read
        // of the slot we are about to reuse happen-before our write to it. Without it we could
        // overwrite a slot the consumer is still reading.
        cached_head_ = head_.load(std::memory_order_acquire);
        if (tail - cached_head_ >= capacity_) return false;  // really full
      }
    } else {
      const uint64_t head = head_.load(std::memory_order_acquire);  // same reason as above
      if (tail - head >= capacity_) return false;
    }

    slots_[tail & mask_] = value;

    // Release: publishes the slot write above. A consumer whose acquire load of tail_ sees
    // tail + 1 is guaranteed to see the data in the slot, not stale bytes.
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer only. Returns false, leaving `out` untouched, if the queue is empty.
  bool try_pop(T& out) {
    // Relaxed: only this thread ever writes head_.
    const uint64_t head = head_.load(std::memory_order_relaxed);

    if constexpr (Cached) {
      if (head == cached_tail_) {  // empty by the stale copy: look at the real tail
        // Acquire: pairs with the producer's release store of tail_. It makes the producer's
        // write of the slot we are about to read happen-before our read of it.
        cached_tail_ = tail_.load(std::memory_order_acquire);
        if (head == cached_tail_) return false;  // really empty
      }
    } else {
      if (head == tail_.load(std::memory_order_acquire)) return false;  // same reason as above
    }

    out = slots_[head & mask_];

    // Release: our read of the slot above must not move after this store, because this store
    // tells the producer the slot is free to overwrite. Pairs with the producer's acquire of head_.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer only. Copies up to `max_items` items into `out` in FIFO order and returns how many
  // (0 if the queue is empty or max_items is 0). One release store covers the whole batch, so the
  // producer is told about all the freed slots at once.
  std::size_t try_pop_batch(T* out, std::size_t max_items) {
    const uint64_t head = head_.load(std::memory_order_relaxed);  // own counter, as in try_pop

    uint64_t available;
    if constexpr (Cached) {
      available = cached_tail_ - head;
      if (available == 0) {
        cached_tail_ = tail_.load(std::memory_order_acquire);  // as in try_pop
        available = cached_tail_ - head;
      }
    } else {
      available = tail_.load(std::memory_order_acquire) - head;
    }

    const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(available, max_items));
    if (n == 0) return 0;
    for (std::size_t i = 0; i < n; ++i) out[i] = slots_[(head + i) & mask_];
    head_.store(head + n, std::memory_order_release);  // as in try_pop, for n slots
    return n;
  }

  // APPROXIMATE. Safe to call from any thread, but the other thread may push or pop at any moment,
  // so the answer can be out of date before it is returned. Never use it to decide whether a
  // push or pop will succeed; call try_push / try_pop and look at the result. Exact only when no
  // other thread is using the queue.
  std::size_t size_approx() const {
    // head first: counters only grow, so a tail read after it is never smaller than head.
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    return static_cast<std::size_t>(std::min<uint64_t>(tail - head, capacity_));
  }
  bool empty_approx() const { return size_approx() == 0; }  // APPROXIMATE, see size_approx

 private:
  static std::size_t checked_capacity(std::size_t capacity) {
    if (capacity == 0 || !std::has_single_bit(capacity)) {
      throw std::invalid_argument("SpscQueue capacity must be a power of two");
    }
    return capacity;
  }

  // Padded: members that start a new line are aligned to 64. Unpadded: no extra alignment, so
  // everything below lands in the same line.
  static constexpr std::size_t kLine = Padded ? kCacheLineBytes : alignof(std::atomic<uint64_t>);

  // Read-only after construction; written by nobody, read by both threads.
  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;

  // Producer's line: its counter and its private copy of head. Written only by the producer.
  alignas(kLine) std::atomic<uint64_t> tail_{0};
  uint64_t cached_head_ = 0;

  // Consumer's line: its counter and its private copy of tail. Written only by the consumer.
  alignas(kLine) std::atomic<uint64_t> head_{0};
  uint64_t cached_tail_ = 0;
};

}  // namespace lob
