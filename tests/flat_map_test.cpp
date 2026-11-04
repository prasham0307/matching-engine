#include "lob/flat_map.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

using lob::FlatMap;

// The first `count` keys, scanning upward from `start`, whose home slot is `home`.
std::vector<uint64_t> keys_with_home(const FlatMap& m, std::size_t home, std::size_t count,
                                     uint64_t start = 1) {
  std::vector<uint64_t> keys;
  for (uint64_t k = start; keys.size() < count; ++k) {
    if (m.home_slot(k) == home) keys.push_back(k);
  }
  return keys;
}

TEST(FlatMap, EmptyMapFindsNothing) {
  FlatMap m;
  EXPECT_EQ(m.size(), 0u);
  EXPECT_EQ(m.find(5), std::nullopt);
  EXPECT_FALSE(m.contains(5));
  EXPECT_FALSE(m.erase(5));
  EXPECT_TRUE(m.check_invariants());
}

TEST(FlatMap, InsertFindAndDuplicate) {
  FlatMap m(100);
  for (uint64_t k = 1; k <= 100; ++k) EXPECT_TRUE(m.insert(k, static_cast<uint32_t>(k * 3)));
  for (uint64_t k = 1; k <= 100; ++k) EXPECT_EQ(m.find(k), k * 3);
  EXPECT_FALSE(m.insert(7, 999));  // duplicate: rejected, original kept
  EXPECT_EQ(m.find(7), 21u);
  EXPECT_EQ(m.size(), 100u);
  EXPECT_TRUE(m.check_invariants());
}

TEST(FlatMap, EraseThenReinsert) {
  FlatMap m;
  m.insert(9, 1);
  m.insert(10, 2);
  EXPECT_TRUE(m.erase(9));
  EXPECT_FALSE(m.erase(9));
  EXPECT_EQ(m.find(9), std::nullopt);
  EXPECT_EQ(m.find(10), 2u);
  EXPECT_TRUE(m.insert(9, 5));
  EXPECT_EQ(m.find(9), 5u);
}

TEST(FlatMap, EveryKeyAndValueIsLegal) {
  FlatMap m;
  EXPECT_TRUE(m.insert(0, 0));
  EXPECT_TRUE(m.insert(UINT64_MAX, UINT32_MAX));
  EXPECT_EQ(m.find(0), 0u);  // value 0 is a real value, not "missing"
  EXPECT_EQ(m.find(UINT64_MAX), UINT32_MAX);
  EXPECT_TRUE(m.erase(0));
  EXPECT_EQ(m.find(0), std::nullopt);
  EXPECT_EQ(m.find(UINT64_MAX), UINT32_MAX);
}

TEST(FlatMap, BackwardShiftKeepsALongChainReachableForEveryErasePosition) {
  constexpr std::size_t kChain = 30;
  for (std::size_t victim = 0; victim < kChain; ++victim) {
    FlatMap m(100);
    const auto keys = keys_with_home(m, 50, kChain);
    for (std::size_t i = 0; i < kChain; ++i) ASSERT_TRUE(m.insert(keys[i], static_cast<uint32_t>(i)));
    ASSERT_TRUE(m.erase(keys[victim]));
    for (std::size_t i = 0; i < kChain; ++i) {
      if (i != victim) {
        EXPECT_EQ(m.find(keys[i]), i) << "victim " << victim << " key " << i;
      }
    }
    EXPECT_EQ(m.find(keys[victim]), std::nullopt);
    EXPECT_TRUE(m.check_invariants()) << "victim " << victim;
    EXPECT_EQ(m.longest_probe(), kChain - 1);
  }
}

TEST(FlatMap, BackwardShiftHandlesAChainThatWrapsAroundTheArray) {
  constexpr std::size_t kChain = 10;
  for (std::size_t victim = 0; victim < kChain; ++victim) {
    FlatMap m(100);  // 256 slots
    const auto keys = keys_with_home(m, m.capacity() - 3, kChain);
    for (std::size_t i = 0; i < kChain; ++i) ASSERT_TRUE(m.insert(keys[i], static_cast<uint32_t>(i)));
    ASSERT_TRUE(m.erase(keys[victim]));
    for (std::size_t i = 0; i < kChain; ++i) {
      if (i != victim) {
        EXPECT_EQ(m.find(keys[i]), i) << "victim " << victim << " key " << i;
      }
    }
    EXPECT_TRUE(m.check_invariants()) << "victim " << victim;
  }
}

TEST(FlatMap, GrowsWhenFullAndCountsResizes) {
  FlatMap m(1);
  EXPECT_EQ(m.resize_count(), 0u);
  for (uint64_t k = 0; k < 5000; ++k) ASSERT_TRUE(m.insert(k, static_cast<uint32_t>(k)));
  EXPECT_GT(m.resize_count(), 0u);
  EXPECT_GT(m.capacity(), 10000u);
  for (uint64_t k = 0; k < 5000; ++k) EXPECT_EQ(m.find(k), k);
  EXPECT_TRUE(m.check_invariants());
}

TEST(FlatMap, PresizedMapNeverResizes) {
  FlatMap m(1000);
  for (uint64_t k = 0; k < 1000; ++k) ASSERT_TRUE(m.insert(k * 7919 + 3, 1));
  EXPECT_EQ(m.resize_count(), 0u);
  EXPECT_EQ(m.capacity(), 2048u);
}

TEST(FlatMap, SteadyChurnDoesNotGrowOrDegrade) {
  FlatMap m(300);
  uint64_t next = 1;
  std::vector<uint64_t> live;
  for (int i = 0; i < 300; ++i) {
    m.insert(next, 0);
    live.push_back(next++);
  }
  const std::size_t capacity = m.capacity();
  std::mt19937_64 rng(5);
  for (int step = 0; step < 100000; ++step) {
    const std::size_t victim = rng() % live.size();
    ASSERT_TRUE(m.erase(live[victim]));
    live[victim] = next++;
    ASSERT_TRUE(m.insert(live[victim], 0));
  }
  EXPECT_EQ(m.capacity(), capacity);
  EXPECT_EQ(m.resize_count(), 0u);
  EXPECT_LT(m.longest_probe(), 24u);
  EXPECT_TRUE(m.check_invariants());
}

TEST(FlatMap, StridedKeysDoNotClusterBadly) {
  for (const uint64_t shift : {10u, 20u, 32u, 40u}) {
    FlatMap m(2000);
    for (uint64_t i = 0; i < 2000; ++i) ASSERT_TRUE(m.insert((i + 1) << shift, 1));
    EXPECT_LT(m.longest_probe(), 40u) << "stride 2^" << shift;
  }
}

TEST(FlatMap, MovedMapKeepsItsEntries) {
  FlatMap a(100);
  for (uint64_t k = 0; k < 20; ++k) a.insert(k, static_cast<uint32_t>(k + 1));
  FlatMap b(std::move(a));
  for (uint64_t k = 0; k < 20; ++k) EXPECT_EQ(b.find(k), k + 1);
}

void run_randomized(std::size_t expected, uint64_t seed) {
  constexpr uint64_t kKeys = 3000;
  FlatMap m(expected);
  std::unordered_map<uint64_t, uint32_t> reference;
  std::mt19937_64 rng(seed);
  for (std::size_t step = 0; step < 200000; ++step) {
    const uint64_t key = rng() % kKeys;
    const uint32_t value = static_cast<uint32_t>(rng());
    switch (rng() % 3) {
      case 0:
        ASSERT_EQ(m.insert(key, value), reference.emplace(key, value).second) << step;
        break;
      case 1:
        ASSERT_EQ(m.erase(key), reference.erase(key) == 1) << step;
        break;
      default: {
        const auto it = reference.find(key);
        const std::optional<uint32_t> expected_value =
            it == reference.end() ? std::nullopt : std::optional<uint32_t>(it->second);
        ASSERT_EQ(m.find(key), expected_value) << step;
        break;
      }
    }
    ASSERT_EQ(m.size(), reference.size()) << step;
    if (step % 5000 == 0) {
      ASSERT_TRUE(m.check_invariants()) << step;
    }
  }
  ASSERT_TRUE(m.check_invariants());
}

TEST(FlatMap, RandomizedAgainstUnorderedMapWithGrowth) { run_randomized(1, 21); }
TEST(FlatMap, RandomizedAgainstUnorderedMapPresized) { run_randomized(4000, 22); }

}  // namespace
