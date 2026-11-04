#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

#include "lob/types.hpp"

namespace lob {

namespace detail {

// Allocator that hands out 64-byte aligned storage, so an array of 16-byte slots starts on a
// cache line and four slots share each line.
template <class T>
struct CacheAlignedAllocator {
  using value_type = T;
  static constexpr std::align_val_t kAlign{64};

  CacheAlignedAllocator() = default;
  template <class U>
  CacheAlignedAllocator(const CacheAlignedAllocator<U>&) {}

  T* allocate(std::size_t n) { return static_cast<T*>(::operator new(n * sizeof(T), kAlign)); }
  void deallocate(T* p, std::size_t) { ::operator delete(p, kAlign); }

  template <class U>
  bool operator==(const CacheAlignedAllocator<U>&) const {
    return true;
  }
};

}  // namespace detail

// Flat open-addressed hash table from order id to Order*, used as the book's id index.
//
// Layout: one contiguous array of {key, value} slots, power-of-two capacity, linear probing. A slot
// is empty when value == nullptr, so every uint64_t (including 0) is a usable key; values must be
// non-null. The load factor is kept strictly under 0.5, so every probe chain ends at an empty slot
// and chains stay short.
//
// Deletion uses backward shift, not tombstones: after removing a slot, later entries of the same
// probe cluster that could legally sit earlier are moved up to fill the hole. Chains therefore
// never accumulate dead entries, however many inserts and erases happen.
//
// Hash: ((id ^ (id >> 32)) * golden) >> (64 - log2(capacity)). std::hash<uint64_t> is the identity
// on libstdc++, so with a power-of-two mask ids that share low bits (multiples of 1024, say) would
// pile into the same slots and linear probing would degrade; unordered_map avoids that with a
// prime modulus, which costs a 64-bit division per lookup. A multiply plus a shift spreads those
// patterns for the price of one multiplication. The high bits of the product are used because the
// low bits of a product depend only on the low bits of the key, and the xor-fold keeps ids that
// differ only above bit 32 from colliding.
//
// Growth: the constructor sizes the table for an expected entry count, and with that many entries
// or fewer the table never allocates again. If an insert would push the load factor to 0.5 the
// table doubles and rehashes everything. That allocates and costs O(capacity): it is a rare slow
// path, not a hot-path operation, and resize_count() exists so callers can confirm it never ran in
// a measured region.
//
// A moved-from IdIndex owns no storage and may only be destroyed or assigned to.
class IdIndex {
 public:
  static constexpr std::size_t kMinCapacity = 16;

  // Sizes the table so `expected_entries` entries fit without growing.
  explicit IdIndex(std::size_t expected_entries = 1024)
      : slots_(capacity_for(expected_entries)) {
    set_geometry();
  }

  IdIndex(IdIndex&&) noexcept = default;
  IdIndex& operator=(IdIndex&&) noexcept = default;
  IdIndex(const IdIndex&) = delete;
  IdIndex& operator=(const IdIndex&) = delete;

  // Returns the Order for `id`, or nullptr if absent.
  Order* find(uint64_t id) const {
    std::size_t i = home_slot(id);
    for (;;) {
      const Slot& s = slots_[i];
      if (s.value == nullptr) return nullptr;
      if (s.key == id) return s.value;
      i = (i + 1) & mask_;
    }
  }

  bool contains(uint64_t id) const { return find(id) != nullptr; }

  // Inserts id -> order. Returns false, changing nothing, if `id` is already present.
  bool insert(uint64_t id, Order* order) {
    std::size_t i = home_slot(id);
    while (slots_[i].value != nullptr) {
      if (slots_[i].key == id) return false;
      i = (i + 1) & mask_;
    }
    if ((size_ + 1) * 2 >= slots_.size()) {  // keep load strictly under 0.5
      grow();                                // rare slow path: allocates and rehashes
      i = home_slot(id);
      while (slots_[i].value != nullptr) i = (i + 1) & mask_;
    }
    slots_[i] = Slot{id, order};
    ++size_;
    return true;
  }

  // Removes `id`. Returns false if it was not present.
  bool erase(uint64_t id) {
    std::size_t i = home_slot(id);
    for (;;) {
      const Slot& s = slots_[i];
      if (s.value == nullptr) return false;
      if (s.key == id) break;
      i = (i + 1) & mask_;
    }

    // Backward shift. i is the hole. Scan the rest of the cluster; an entry at j may move into
    // the hole iff its home slot is not cyclically inside (i, j], i.e. iff the hole lies on its
    // probe path between its home and j.
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].value == nullptr) break;
      const std::size_t k = home_slot(slots_[j].key);
      if (((j - k) & mask_) >= ((j - i) & mask_)) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i] = Slot{};
    --size_;
    return true;
  }

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return slots_.size(); }
  // Number of times the table has grown. Zero means no hot-path allocation ever happened.
  std::size_t resize_count() const { return resizes_; }

  // Calls f(key, order) for every entry, in table order.
  template <class F>
  void for_each(F&& f) const {
    for (const Slot& s : slots_) {
      if (s.value != nullptr) f(s.key, s.value);
    }
  }

  // The slot an id hashes to before probing. Exposed for tests and diagnostics.
  std::size_t home_slot(uint64_t id) const {
    return static_cast<std::size_t>(((id ^ (id >> 32)) * kMultiplier) >> shift_);
  }

  // Longest probe sequence (home slot to actual slot, plus one) over all entries. O(capacity).
  std::size_t longest_probe() const {
    std::size_t longest = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].value == nullptr) continue;
      const std::size_t dist = ((i - home_slot(slots_[i].key)) & mask_) + 1;
      if (dist > longest) longest = dist;
    }
    return longest;
  }

  // Checks the structural invariants. O(capacity); for tests and debugging only.
  bool check_invariants() const {
    const std::size_t cap = slots_.size();
    if (cap < kMinCapacity || !std::has_single_bit(cap)) return false;
    if (mask_ != cap - 1 || shift_ != static_cast<unsigned>(64 - std::countr_zero(cap))) return false;
    if (size_ * 2 >= cap) return false;  // load strictly under 0.5

    std::size_t occupied = 0;
    for (std::size_t i = 0; i < cap; ++i) {
      const Slot& s = slots_[i];
      if (s.value == nullptr) continue;
      ++occupied;
      // Reachable: probing from the home slot must stop at this very slot, with no empty slot
      // on the way (which is what backward shift has to preserve).
      std::size_t p = home_slot(s.key);
      while (p != i) {
        if (slots_[p].value == nullptr || slots_[p].key == s.key) return false;
        p = (p + 1) & mask_;
      }
    }
    if (occupied != size_) return false;
    // There must be an empty slot, or find() would never terminate.
    return occupied < cap;
  }

 private:
  struct Slot {
    uint64_t key = 0;
    Order* value = nullptr;
  };
  static_assert(sizeof(Slot) == 16, "four slots per cache line");

  static constexpr uint64_t kMultiplier = 0x9E3779B97F4A7C15ULL;  // 2^64 / golden ratio

  // Smallest power-of-two capacity, at least kMinCapacity, that holds `entries` under load 0.5.
  static std::size_t capacity_for(std::size_t entries) {
    return std::bit_ceil(std::max<std::size_t>(kMinCapacity, 2 * entries + 1));
  }

  void set_geometry() {
    mask_ = slots_.size() - 1;
    shift_ = static_cast<unsigned>(64 - std::countr_zero(slots_.size()));
  }

  void grow() {
    std::vector<Slot, detail::CacheAlignedAllocator<Slot>> old = std::move(slots_);
    slots_ = std::vector<Slot, detail::CacheAlignedAllocator<Slot>>(old.size() * 2);
    set_geometry();
    for (const Slot& s : old) {
      if (s.value == nullptr) continue;
      std::size_t i = home_slot(s.key);
      while (slots_[i].value != nullptr) i = (i + 1) & mask_;
      slots_[i] = s;
    }
    ++resizes_;
  }

  std::vector<Slot, detail::CacheAlignedAllocator<Slot>> slots_;
  std::size_t mask_ = 0;
  unsigned shift_ = 0;
  std::size_t size_ = 0;
  std::size_t resizes_ = 0;
};

}  // namespace lob
