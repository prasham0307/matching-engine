#include "lob/id_index.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

using lob::IdIndex;
using lob::Order;

// Distinct Orders so their addresses can stand in for real orders.
std::vector<Order> make_orders(std::size_t n) { return std::vector<Order>(n); }

// The first `count` keys, scanning upward from `start`, whose home slot in `idx` is `home`.
std::vector<uint64_t> keys_with_home(const IdIndex& idx, std::size_t home, std::size_t count,
                                     uint64_t start = 1) {
  std::vector<uint64_t> keys;
  for (uint64_t k = start; keys.size() < count; ++k) {
    if (idx.home_slot(k) == home) keys.push_back(k);
  }
  return keys;
}

TEST(IdIndex, EmptyIndexFindsNothing) {
  IdIndex idx;
  EXPECT_EQ(idx.size(), 0u);
  EXPECT_EQ(idx.find(5), nullptr);
  EXPECT_FALSE(idx.contains(5));
  EXPECT_FALSE(idx.erase(5));
  EXPECT_TRUE(idx.check_invariants());
}

TEST(IdIndex, InsertThenFind) {
  IdIndex idx(200);
  auto orders = make_orders(100);
  for (uint64_t k = 1; k <= 100; ++k) EXPECT_TRUE(idx.insert(k, &orders[k - 1]));
  EXPECT_EQ(idx.size(), 100u);
  for (uint64_t k = 1; k <= 100; ++k) EXPECT_EQ(idx.find(k), &orders[k - 1]);
  EXPECT_EQ(idx.find(101), nullptr);
  EXPECT_TRUE(idx.check_invariants());
}

TEST(IdIndex, DuplicateInsertIsRejectedAndKeepsTheOriginal) {
  IdIndex idx;
  auto orders = make_orders(2);
  EXPECT_TRUE(idx.insert(7, &orders[0]));
  EXPECT_FALSE(idx.insert(7, &orders[1]));
  EXPECT_EQ(idx.find(7), &orders[0]);
  EXPECT_EQ(idx.size(), 1u);
}

TEST(IdIndex, EraseRemovesOnlyThatKey) {
  IdIndex idx;
  auto orders = make_orders(3);
  idx.insert(1, &orders[0]);
  idx.insert(2, &orders[1]);
  idx.insert(3, &orders[2]);
  EXPECT_TRUE(idx.erase(2));
  EXPECT_EQ(idx.find(2), nullptr);
  EXPECT_EQ(idx.find(1), &orders[0]);
  EXPECT_EQ(idx.find(3), &orders[2]);
  EXPECT_FALSE(idx.erase(2));  // already gone
  EXPECT_EQ(idx.size(), 2u);
  EXPECT_TRUE(idx.check_invariants());
}

TEST(IdIndex, ErasedKeyCanBeInsertedAgain) {
  IdIndex idx;
  auto orders = make_orders(2);
  idx.insert(9, &orders[0]);
  idx.erase(9);
  EXPECT_TRUE(idx.insert(9, &orders[1]));
  EXPECT_EQ(idx.find(9), &orders[1]);
  EXPECT_EQ(idx.size(), 1u);
}

TEST(IdIndex, ZeroAndMaxKeysAreValid) {
  IdIndex idx;
  auto orders = make_orders(2);
  const uint64_t max = UINT64_MAX;
  EXPECT_TRUE(idx.insert(0, &orders[0]));
  EXPECT_TRUE(idx.insert(max, &orders[1]));
  EXPECT_EQ(idx.find(0), &orders[0]);
  EXPECT_EQ(idx.find(max), &orders[1]);
  EXPECT_TRUE(idx.erase(0));
  EXPECT_EQ(idx.find(0), nullptr);
  EXPECT_EQ(idx.find(max), &orders[1]);
  EXPECT_TRUE(idx.check_invariants());
}

TEST(IdIndex, CapacityIsAPowerOfTwoSizedForTheExpectedEntries) {
  for (const std::size_t expected : {0u, 1u, 7u, 8u, 15u, 16u, 100u, 1000u, 65536u}) {
    IdIndex idx(expected);
    const std::size_t cap = idx.capacity();
    EXPECT_EQ(cap & (cap - 1), 0u) << expected;
    EXPECT_GE(cap, IdIndex::kMinCapacity) << expected;
    EXPECT_GT(cap, 2 * expected) << "load must stay under 0.5 at " << expected << " entries";
  }
  EXPECT_EQ(IdIndex(1000).capacity(), 2048u);
}

TEST(IdIndex, CollidingKeysFormAChainAndAllResolve) {
  IdIndex idx(100);  // 256 slots
  const auto keys = keys_with_home(idx, 10, 8);
  auto orders = make_orders(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) ASSERT_TRUE(idx.insert(keys[i], &orders[i]));

  for (std::size_t i = 0; i < keys.size(); ++i) EXPECT_EQ(idx.find(keys[i]), &orders[i]);
  EXPECT_EQ(idx.longest_probe(), keys.size());  // 8 keys, one home slot: the last probes 8 times
  EXPECT_TRUE(idx.check_invariants());
}

// Erase each position of a long same-home chain in turn. Backward shift must keep every
// remaining key reachable, whichever slot of the chain the hole opens in.
TEST(IdIndex, BackwardShiftKeepsALongChainReachableForEveryErasePosition) {
  constexpr std::size_t kChain = 40;
  for (std::size_t victim = 0; victim < kChain; ++victim) {
    IdIndex idx(100);
    const auto keys = keys_with_home(idx, 50, kChain);
    auto orders = make_orders(kChain);
    for (std::size_t i = 0; i < kChain; ++i) ASSERT_TRUE(idx.insert(keys[i], &orders[i]));

    ASSERT_TRUE(idx.erase(keys[victim]));
    EXPECT_EQ(idx.find(keys[victim]), nullptr) << "victim " << victim;
    for (std::size_t i = 0; i < kChain; ++i) {
      if (i != victim) {
        EXPECT_EQ(idx.find(keys[i]), &orders[i]) << "victim " << victim << " key " << i;
      }
    }
    EXPECT_TRUE(idx.check_invariants()) << "victim " << victim;
    EXPECT_EQ(idx.longest_probe(), kChain - 1) << "chain should have closed up, victim " << victim;
  }
}

TEST(IdIndex, BackwardShiftHandlesAChainThatWrapsAroundTheArray) {
  constexpr std::size_t kChain = 12;
  for (std::size_t victim = 0; victim < kChain; ++victim) {
    IdIndex idx(100);  // 256 slots
    const auto keys = keys_with_home(idx, idx.capacity() - 4, kChain);  // chain crosses slot 255 -> 0
    auto orders = make_orders(kChain);
    for (std::size_t i = 0; i < kChain; ++i) ASSERT_TRUE(idx.insert(keys[i], &orders[i]));

    ASSERT_TRUE(idx.erase(keys[victim]));
    for (std::size_t i = 0; i < kChain; ++i) {
      if (i != victim) {
        EXPECT_EQ(idx.find(keys[i]), &orders[i]) << "victim " << victim << " key " << i;
      }
    }
    EXPECT_TRUE(idx.check_invariants()) << "victim " << victim;
  }
}

// Neighbouring home slots whose clusters run into each other: an entry may only move up into a
// hole if the hole is on its own probe path, never past its home slot.
TEST(IdIndex, BackwardShiftNeverMovesAnEntryBeforeItsHome) {
  IdIndex idx(100);
  std::vector<uint64_t> keys;
  for (std::size_t round = 0; round < 4; ++round) {
    for (std::size_t home = 20; home < 24; ++home) {
      // A fresh key for this home each round; scanning from distinct starts keeps them apart.
      keys.push_back(keys_with_home(idx, home, 1, 1 + round * 100000)[0]);
    }
  }
  auto orders = make_orders(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) ASSERT_TRUE(idx.insert(keys[i], &orders[i]));

  for (std::size_t i = 0; i < keys.size(); i += 2) {
    ASSERT_TRUE(idx.erase(keys[i]));
    EXPECT_TRUE(idx.check_invariants()) << "after erasing " << i;
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    EXPECT_EQ(idx.find(keys[i]), i % 2 == 0 ? nullptr : &orders[i]) << i;
  }
}

TEST(IdIndex, GrowsWhenFullAndCountsResizes) {
  IdIndex idx(1);  // 16 slots, grows when the 8th entry arrives
  auto orders = make_orders(5000);
  EXPECT_EQ(idx.resize_count(), 0u);
  for (uint64_t k = 0; k < 5000; ++k) ASSERT_TRUE(idx.insert(k + 1, &orders[k]));

  EXPECT_GT(idx.resize_count(), 0u);
  EXPECT_EQ(idx.size(), 5000u);
  EXPECT_GT(idx.capacity(), 10000u);  // load under 0.5
  for (uint64_t k = 0; k < 5000; ++k) EXPECT_EQ(idx.find(k + 1), &orders[k]);
  EXPECT_TRUE(idx.check_invariants());

  const std::size_t resizes = idx.resize_count();
  for (uint64_t k = 0; k < 5000; ++k) idx.erase(k + 1);
  EXPECT_EQ(idx.size(), 0u);
  EXPECT_EQ(idx.resize_count(), resizes);  // erasing never shrinks or rehashes
}

TEST(IdIndex, PresizedIndexNeverResizes) {
  IdIndex idx(1000);
  auto orders = make_orders(1000);
  for (uint64_t k = 0; k < 1000; ++k) ASSERT_TRUE(idx.insert(k * 7919 + 3, &orders[k]));
  EXPECT_EQ(idx.resize_count(), 0u);
  EXPECT_EQ(idx.capacity(), 2048u);
  EXPECT_TRUE(idx.check_invariants());
}

// Churn at a steady size, like a book where orders keep arriving and filling: with backward
// shift the table must not degrade or grow, however long it runs.
TEST(IdIndex, SteadyChurnDoesNotGrowOrDegrade) {
  IdIndex idx(300);
  auto orders = make_orders(300);
  uint64_t next = 1;
  std::vector<uint64_t> live;
  for (std::size_t i = 0; i < 300; ++i) {
    idx.insert(next, &orders[i]);
    live.push_back(next++);
  }
  const std::size_t capacity = idx.capacity();
  std::mt19937_64 rng(5);
  for (std::size_t step = 0; step < 200000; ++step) {
    const std::size_t victim = rng() % live.size();
    ASSERT_TRUE(idx.erase(live[victim]));
    live[victim] = next++;
    ASSERT_TRUE(idx.insert(live[victim], &orders[victim]));
  }
  EXPECT_EQ(idx.capacity(), capacity);
  EXPECT_EQ(idx.resize_count(), 0u);
  EXPECT_LT(idx.longest_probe(), 24u);  // no tombstone build-up
  EXPECT_TRUE(idx.check_invariants());
}

// Patterns that would all land in a handful of slots under an identity hash with a power of two
// mask. The mix must spread them.
TEST(IdIndex, StridedKeysDoNotClusterBadly) {
  for (const uint64_t shift : {10u, 20u, 32u, 40u, 50u}) {
    IdIndex idx(2000);  // 4096 slots, 2000 entries
    auto orders = make_orders(2000);
    for (uint64_t i = 0; i < 2000; ++i) ASSERT_TRUE(idx.insert((i + 1) << shift, &orders[i]));
    EXPECT_LT(idx.longest_probe(), 40u) << "stride 2^" << shift;
    EXPECT_TRUE(idx.check_invariants()) << "stride 2^" << shift;
  }
}

TEST(IdIndex, ForEachVisitsEveryEntryOnce) {
  IdIndex idx;
  auto orders = make_orders(50);
  for (uint64_t k = 0; k < 50; ++k) idx.insert(100 + k, &orders[k]);
  std::vector<uint64_t> seen;
  idx.for_each([&](uint64_t key, const Order* o) {
    seen.push_back(key);
    EXPECT_EQ(o, &orders[key - 100]);
  });
  std::sort(seen.begin(), seen.end());
  ASSERT_EQ(seen.size(), 50u);
  for (uint64_t k = 0; k < 50; ++k) EXPECT_EQ(seen[k], 100 + k);
}

TEST(IdIndex, MovedIndexKeepsItsEntries) {
  IdIndex a(100);
  auto orders = make_orders(20);
  for (uint64_t k = 0; k < 20; ++k) a.insert(k + 1, &orders[k]);
  IdIndex b(std::move(a));
  for (uint64_t k = 0; k < 20; ++k) EXPECT_EQ(b.find(k + 1), &orders[k]);
  IdIndex c;
  c = std::move(b);
  EXPECT_EQ(c.size(), 20u);
  EXPECT_TRUE(c.check_invariants());
}

// Random inserts, erases and lookups checked against std::unordered_map. The small key space
// forces constant collisions, reinsertions of erased keys and long clusters.
void run_randomized(std::size_t expected_entries, uint64_t seed) {
  constexpr uint64_t kKeys = 3000;
  constexpr std::size_t kSteps = 300000;
  IdIndex idx(expected_entries);
  std::unordered_map<uint64_t, Order*> reference;
  auto orders = make_orders(kKeys);
  std::mt19937_64 rng(seed);

  for (std::size_t step = 0; step < kSteps; ++step) {
    const uint64_t key = rng() % kKeys;
    switch (rng() % 3) {
      case 0: {
        const bool inserted = idx.insert(key, &orders[key]);
        const bool ref_inserted = reference.emplace(key, &orders[key]).second;
        ASSERT_EQ(inserted, ref_inserted) << "insert " << key << " step " << step;
        break;
      }
      case 1: {
        const bool erased = idx.erase(key);
        const bool ref_erased = reference.erase(key) == 1;
        ASSERT_EQ(erased, ref_erased) << "erase " << key << " step " << step;
        break;
      }
      default: {
        const auto it = reference.find(key);
        ASSERT_EQ(idx.find(key), it == reference.end() ? nullptr : it->second)
            << "find " << key << " step " << step;
        break;
      }
    }
    ASSERT_EQ(idx.size(), reference.size()) << "step " << step;
    if (step % 5000 == 0) {
      ASSERT_TRUE(idx.check_invariants()) << "step " << step;
    }
  }
  ASSERT_TRUE(idx.check_invariants());
  for (uint64_t key = 0; key < kKeys; ++key) {
    const auto it = reference.find(key);
    EXPECT_EQ(idx.find(key), it == reference.end() ? nullptr : it->second) << key;
  }
}

TEST(IdIndex, RandomizedAgainstUnorderedMapWithGrowth) { run_randomized(1, 11); }
TEST(IdIndex, RandomizedAgainstUnorderedMapPresized) { run_randomized(4000, 12); }
TEST(IdIndex, RandomizedAgainstUnorderedMapOtherSeed) { run_randomized(64, 13); }

}  // namespace
