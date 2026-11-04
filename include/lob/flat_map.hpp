#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace lob {

// Flat open-addressed hash table from uint64_t to uint32_t, used by MatchingEngine for
// order id -> symbol and symbol -> book index.
//
// Same design as IdIndex: one contiguous array of 16-byte slots, power-of-two capacity, linear
// probing, load factor kept strictly under 0.5, backward-shift deletion (no tombstones), and a
// multiply hash (std::hash<uint64_t> is the identity on libstdc++, which clusters badly under a
// power-of-two mask). It stores a value instead of an Order*, so it needs a separate "used" flag
// and every uint64_t key and uint32_t value is legal.
//
// The constructor sizes the table for an expected entry count; with that many entries or fewer it
// never allocates again. If an insert would push the load factor to 0.5 the table doubles and
// rehashes. That allocates and is O(capacity): a rare slow path, counted by resize_count().
//
// A moved-from FlatMap owns no storage and may only be destroyed or assigned to.
class FlatMap {
 public:
  static constexpr std::size_t kMinCapacity = 16;

  explicit FlatMap(std::size_t expected_entries = 16) : slots_(capacity_for(expected_entries)) {
    set_geometry();
  }

  FlatMap(FlatMap&&) noexcept = default;
  FlatMap& operator=(FlatMap&&) noexcept = default;
  FlatMap(const FlatMap&) = delete;
  FlatMap& operator=(const FlatMap&) = delete;

  std::optional<uint32_t> find(uint64_t key) const {
    std::size_t i = home_slot(key);
    for (;;) {
      const Slot& s = slots_[i];
      if (s.used == 0) return std::nullopt;
      if (s.key == key) return s.value;
      i = (i + 1) & mask_;
    }
  }

  bool contains(uint64_t key) const { return find(key).has_value(); }

  // Inserts key -> value. Returns false, changing nothing, if the key is already present.
  bool insert(uint64_t key, uint32_t value) {
    std::size_t i = home_slot(key);
    while (slots_[i].used != 0) {
      if (slots_[i].key == key) return false;
      i = (i + 1) & mask_;
    }
    if ((size_ + 1) * 2 >= slots_.size()) {  // keep load strictly under 0.5
      grow();                                // rare slow path: allocates and rehashes
      i = home_slot(key);
      while (slots_[i].used != 0) i = (i + 1) & mask_;
    }
    slots_[i] = Slot{key, value, 1};
    ++size_;
    return true;
  }

  // Removes `key`. Returns false if it was not present.
  bool erase(uint64_t key) {
    std::size_t i = home_slot(key);
    for (;;) {
      const Slot& s = slots_[i];
      if (s.used == 0) return false;
      if (s.key == key) break;
      i = (i + 1) & mask_;
    }

    // Backward shift: i is the hole. An entry further along the cluster may move into it iff its
    // home slot is not cyclically inside (i, j], i.e. iff the hole is on its probe path.
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].used == 0) break;
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
  std::size_t resize_count() const { return resizes_; }

  // Calls f(key, value) for every entry, in table order.
  template <class F>
  void for_each(F&& f) const {
    for (const Slot& s : slots_) {
      if (s.used != 0) f(s.key, s.value);
    }
  }

  // The slot a key hashes to before probing. Exposed for tests and diagnostics.
  std::size_t home_slot(uint64_t key) const {
    return static_cast<std::size_t>(((key ^ (key >> 32)) * kMultiplier) >> shift_);
  }

  // Longest probe sequence over all entries. O(capacity); for tests and diagnostics.
  std::size_t longest_probe() const {
    std::size_t longest = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].used == 0) continue;
      longest = std::max(longest, ((i - home_slot(slots_[i].key)) & mask_) + 1);
    }
    return longest;
  }

  // Checks the structural invariants. O(capacity); for tests and debugging only.
  bool check_invariants() const {
    const std::size_t cap = slots_.size();
    if (cap < kMinCapacity || !std::has_single_bit(cap)) return false;
    if (mask_ != cap - 1 || shift_ != static_cast<unsigned>(64 - std::countr_zero(cap))) return false;
    if (size_ * 2 >= cap) return false;

    std::size_t occupied = 0;
    for (std::size_t i = 0; i < cap; ++i) {
      if (slots_[i].used == 0) continue;
      ++occupied;
      // Probing from the home slot must stop at this slot, with no empty slot on the way.
      std::size_t p = home_slot(slots_[i].key);
      while (p != i) {
        if (slots_[p].used == 0 || slots_[p].key == slots_[i].key) return false;
        p = (p + 1) & mask_;
      }
    }
    return occupied == size_;
  }

 private:
  struct Slot {
    uint64_t key = 0;
    uint32_t value = 0;
    uint32_t used = 0;  // 1 when the slot holds an entry
  };
  static_assert(sizeof(Slot) == 16);

  static constexpr uint64_t kMultiplier = 0x9E3779B97F4A7C15ULL;

  static std::size_t capacity_for(std::size_t entries) {
    return std::bit_ceil(std::max<std::size_t>(kMinCapacity, 2 * entries + 1));
  }

  void set_geometry() {
    mask_ = slots_.size() - 1;
    shift_ = static_cast<unsigned>(64 - std::countr_zero(slots_.size()));
  }

  void grow() {
    std::vector<Slot> old = std::move(slots_);
    slots_ = std::vector<Slot>(old.size() * 2);
    set_geometry();
    for (const Slot& s : old) {
      if (s.used == 0) continue;
      std::size_t i = home_slot(s.key);
      while (slots_[i].used != 0) i = (i + 1) & mask_;
      slots_[i] = s;
    }
    ++resizes_;
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  unsigned shift_ = 0;
  std::size_t size_ = 0;
  std::size_t resizes_ = 0;
};

}  // namespace lob
