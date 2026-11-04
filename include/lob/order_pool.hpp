#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "lob/types.hpp"

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

namespace lob {

// Slab allocator for Order objects.
//
// Orders live in fixed-size chunks that are never moved or freed until the pool is destroyed, so
// an Order* stays valid for as long as the order is allocated (the book's intrusive list links
// and id index rely on this). Free slots form a singly linked list threaded through the unused
// Order::next field. allocate() pops the list head, release() pushes a slot back, so once the
// pool has grown to the book's peak size there is no heap traffic per order. A chunk is only
// added when the free list is empty.
//
// Single threaded, like the book that owns it.
class OrderPool {
 public:
  static constexpr std::size_t kDefaultChunkOrders = 4096;

  explicit OrderPool(std::size_t chunk_orders = kDefaultChunkOrders)
      : chunk_orders_(chunk_orders == 0 ? 1 : chunk_orders) {}

  ~OrderPool() { free_chunks(); }

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;

  // Moving transfers the chunks, so every Order* handed out stays valid in the new owner.
  // The moved-from pool is left empty.
  OrderPool(OrderPool&& other) noexcept
      : chunk_orders_(other.chunk_orders_),
        chunks_(std::move(other.chunks_)),
        free_head_(std::exchange(other.free_head_, nullptr)),
        capacity_(std::exchange(other.capacity_, 0)),
        in_use_(std::exchange(other.in_use_, 0)) {
    other.chunks_.clear();
  }

  OrderPool& operator=(OrderPool&& other) noexcept {
    if (this != &other) {
      free_chunks();
      chunk_orders_ = other.chunk_orders_;
      chunks_ = std::move(other.chunks_);
      other.chunks_.clear();
      free_head_ = std::exchange(other.free_head_, nullptr);
      capacity_ = std::exchange(other.capacity_, 0);
      in_use_ = std::exchange(other.in_use_, 0);
    }
    return *this;
  }

  // Returns a slot holding a copy of `init`. The address is stable until release().
  Order* allocate(const Order& init) {
    if (free_head_ == nullptr) add_chunk();
    Order* o = free_head_;
    free_head_ = o->next;
    unpoison(o);
    *o = init;
    ++in_use_;
    return o;
  }

  // Returns a slot to the free list. The order must already be unlinked from any level.
  void release(Order* o) {
    o->next = free_head_;
    free_head_ = o;
    poison(o);
    --in_use_;
  }

  std::size_t in_use() const { return in_use_; }
  std::size_t capacity() const { return capacity_; }
  std::size_t chunk_count() const { return chunks_.size(); }

 private:
  // Everything before Order::next is poisoned while a slot is free (ASan builds only), so a read
  // of a released order is reported like a use-after-free. Order::next carries the free list.
  static constexpr std::size_t kPoisonBytes = offsetof(Order, next);

  static void poison([[maybe_unused]] Order* o) {
#if defined(__SANITIZE_ADDRESS__)
    ASAN_POISON_MEMORY_REGION(o, kPoisonBytes);
#endif
  }

  static void unpoison([[maybe_unused]] Order* o) {
#if defined(__SANITIZE_ADDRESS__)
    ASAN_UNPOISON_MEMORY_REGION(o, kPoisonBytes);
#endif
  }

  void add_chunk() {
    chunks_.push_back(std::make_unique<Order[]>(chunk_orders_));
    Order* base = chunks_.back().get();
    // Thread the new slots onto the free list in address order.
    for (std::size_t i = chunk_orders_; i-- > 0;) {
      base[i].next = free_head_;
      free_head_ = &base[i];
      poison(&base[i]);
    }
    capacity_ += chunk_orders_;
  }

  void free_chunks() {
#if defined(__SANITIZE_ADDRESS__)
    for (const auto& chunk : chunks_) {
      ASAN_UNPOISON_MEMORY_REGION(chunk.get(), chunk_orders_ * sizeof(Order));
    }
#endif
    chunks_.clear();
    free_head_ = nullptr;
    capacity_ = 0;
    in_use_ = 0;
  }

  std::size_t chunk_orders_;
  std::vector<std::unique_ptr<Order[]>> chunks_;
  Order* free_head_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t in_use_ = 0;
};

}  // namespace lob
