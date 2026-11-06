#include "lob/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using lob::SpscQueue;

// ---- single-thread tests, run on every layout variant ----

template <class Q>
class SpscUnit : public ::testing::Test {};

using Variants = ::testing::Types<SpscQueue<uint64_t, true, true>,    // cached, padded (default)
                                  SpscQueue<uint64_t, false, true>,   // uncached
                                  SpscQueue<uint64_t, true, false>,   // unpadded
                                  SpscQueue<uint64_t, false, false>>; // both off
TYPED_TEST_SUITE(SpscUnit, Variants);

TYPED_TEST(SpscUnit, CapacityMustBeAPowerOfTwo) {
  for (const std::size_t bad : {0u, 3u, 5u, 6u, 7u, 12u, 100u, 1000u}) {
    EXPECT_THROW(TypeParam q(bad), std::invalid_argument) << bad;
  }
  for (const std::size_t good : {1u, 2u, 4u, 64u, 1024u}) {
    TypeParam q(good);
    EXPECT_EQ(q.capacity(), good);
  }
}

TYPED_TEST(SpscUnit, EmptyQueueHasNothingToPop) {
  TypeParam q(8);
  uint64_t v = 777;
  EXPECT_FALSE(q.try_pop(v));
  EXPECT_EQ(v, 777u);  // a failed pop leaves the output alone
  std::array<uint64_t, 4> buf{};
  EXPECT_EQ(q.try_pop_batch(buf.data(), buf.size()), 0u);
  EXPECT_EQ(q.size_approx(), 0u);
  EXPECT_TRUE(q.empty_approx());
}

TYPED_TEST(SpscUnit, FullQueueRejectsPushAndRecoversAfterAPop) {
  TypeParam q(4);
  for (uint64_t i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i)) << i;
  EXPECT_FALSE(q.try_push(99));  // all 4 slots are usable, the 5th push fails
  EXPECT_EQ(q.size_approx(), 4u);
  EXPECT_FALSE(q.empty_approx());

  uint64_t v = 0;
  ASSERT_TRUE(q.try_pop(v));
  EXPECT_EQ(v, 0u);
  EXPECT_TRUE(q.try_push(4));    // one slot is free again
  EXPECT_FALSE(q.try_push(5));   // and full again
  for (uint64_t expected = 1; expected <= 4; ++expected) {
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, expected);  // 99 was never stored
  }
  EXPECT_FALSE(q.try_pop(v));
}

TYPED_TEST(SpscUnit, CapacityOneWorks) {
  TypeParam q(1);
  uint64_t v = 0;
  for (uint64_t i = 0; i < 100; ++i) {
    EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(i + 1000));
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, i);
    EXPECT_FALSE(q.try_pop(v));
  }
}

TYPED_TEST(SpscUnit, FifoOrderAcrossWraparoundManyTimesTheCapacity) {
  TypeParam q(8);
  uint64_t next_push = 0;
  uint64_t next_pop = 0;
  // Fill by a varying amount, then drain by a varying amount, so the read and write positions
  // wrap past the end of the slot array at every possible offset, many times over.
  for (int round = 0; round < 20000; ++round) {
    const std::size_t pushes = 1 + static_cast<std::size_t>(round % 8);
    for (std::size_t i = 0; i < pushes; ++i) {
      if (q.try_push(next_push)) ++next_push;
    }
    const std::size_t pops = 1 + static_cast<std::size_t>((round * 5) % 8);
    for (std::size_t i = 0; i < pops; ++i) {
      uint64_t v;
      if (q.try_pop(v)) {
        ASSERT_EQ(v, next_pop) << "round " << round;
        ++next_pop;
      }
    }
    ASSERT_EQ(q.size_approx(), next_push - next_pop);
  }
  EXPECT_GT(next_push, 8u * 1000u);  // wrapped far more than once
  uint64_t v;
  while (q.try_pop(v)) {
    ASSERT_EQ(v, next_pop++);
  }
  EXPECT_EQ(next_pop, next_push);
}

TYPED_TEST(SpscUnit, BatchPopCopiesUpToKItemsInOrder) {
  TypeParam q(16);
  for (uint64_t i = 0; i < 10; ++i) ASSERT_TRUE(q.try_push(i));

  std::array<uint64_t, 32> buf{};
  EXPECT_EQ(q.try_pop_batch(buf.data(), 0), 0u);  // asking for none gives none
  EXPECT_EQ(q.size_approx(), 10u);

  ASSERT_EQ(q.try_pop_batch(buf.data(), 4), 4u);
  for (uint64_t i = 0; i < 4; ++i) EXPECT_EQ(buf[i], i);
  ASSERT_EQ(q.try_pop_batch(buf.data(), 32), 6u);  // only 6 left, fewer than asked for
  for (uint64_t i = 0; i < 6; ++i) EXPECT_EQ(buf[i], 4 + i);
  EXPECT_EQ(q.try_pop_batch(buf.data(), 32), 0u);
  EXPECT_EQ(q.size_approx(), 0u);
}

TYPED_TEST(SpscUnit, BatchPopWorksAcrossTheWrapPointAndMixesWithSinglePops) {
  TypeParam q(8);
  uint64_t next_push = 0;
  uint64_t next_pop = 0;
  std::array<uint64_t, 8> buf{};
  for (int round = 0; round < 5000; ++round) {
    while (q.try_push(next_push)) ++next_push;  // fill it completely
    if (round % 3 == 0) {
      uint64_t v;
      ASSERT_TRUE(q.try_pop(v));
      ASSERT_EQ(v, next_pop++);
    }
    const std::size_t k = 1 + static_cast<std::size_t>(round % 8);
    const std::size_t got = q.try_pop_batch(buf.data(), k);
    ASSERT_GT(got, 0u);
    ASSERT_LE(got, k);
    for (std::size_t i = 0; i < got; ++i) ASSERT_EQ(buf[i], next_pop++) << "round " << round;
  }
}

TYPED_TEST(SpscUnit, SizeIsExactWhenOnlyOneThreadUsesTheQueue) {
  TypeParam q(16);
  for (uint64_t i = 1; i <= 16; ++i) {
    q.try_push(i);
    EXPECT_EQ(q.size_approx(), i);
  }
  uint64_t v;
  for (uint64_t i = 15; i > 0; --i) {
    q.try_pop(v);
    EXPECT_EQ(q.size_approx(), i);
  }
}

TEST(SpscUnitStruct, WorksWithAStructPayload) {
  struct Item {
    uint32_t a = 0;
    uint64_t b = 0;
    char c = 0;
  };
  SpscQueue<Item> q(4);
  EXPECT_TRUE(q.try_push(Item{1, 2, 'x'}));
  Item out;
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out.a, 1u);
  EXPECT_EQ(out.b, 2u);
  EXPECT_EQ(out.c, 'x');
}

TEST(SpscUnitLayout, PaddedQueueKeepsTheTwoCountersOnDifferentCacheLines) {
  // Can't read private members, but the size says it: three lines (read-only, producer, consumer)
  // when padded, one line when not.
  EXPECT_EQ(sizeof(SpscQueue<uint64_t, true, true>), 3 * lob::kCacheLineBytes);
  EXPECT_EQ(sizeof(SpscQueue<uint64_t, true, false>), lob::kCacheLineBytes);
  EXPECT_EQ(alignof(SpscQueue<uint64_t, true, true>), lob::kCacheLineBytes);
}

// ---- two-thread stress tests ----

inline void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#else
  std::this_thread::yield();
#endif
}

// Items per stress run. 50 million in an optimized build; smaller where each operation is slow.
uint64_t stress_n() {
  if (const char* env = std::getenv("SPSC_STRESS_N")) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(env, &end, 10);
    if (end != env && *end == '\0' && v > 0) return v;
  }
#if defined(__SANITIZE_THREAD__)
  return 400'000;
#elif defined(NDEBUG)
  return 50'000'000;
#else
  return 2'000'000;
#endif
}

// How a stress run went: kept as plain data so the checks happen on the test thread.
struct StressResult {
  uint64_t received = 0;
  uint64_t sum = 0;
  uint64_t first_out_of_order = UINT64_MAX;  // index of the first item that was not the expected one
  uint64_t first_bad_value = 0;
  bool stalled = false;                      // gave up waiting: items were lost
};

// Producer pushes 0, 1, 2 ... n-1. The consumer (this thread) checks every item arrives exactly
// once and in order, and sums them.
template <class Q, bool Batch>
StressResult run_stress(std::size_t capacity, uint64_t n) {
  Q q(capacity);
  std::atomic<bool> go{false};
  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (uint64_t i = 0; i < n; ++i) {
      while (!q.try_push(i)) cpu_relax();
    }
  });

  StressResult r;
  std::array<uint64_t, 32> batch{};
  uint64_t idle = 0;
  go.store(true, std::memory_order_release);
  while (r.received < n) {
    std::size_t got = 0;
    if constexpr (Batch) {
      got = q.try_pop_batch(batch.data(), batch.size());
    } else {
      uint64_t v;
      if (q.try_pop(v)) {
        batch[0] = v;
        got = 1;
      }
    }
    if (got == 0) {
      cpu_relax();
      if (++idle > 400'000'000) {  // tens of seconds with nothing arriving: an item was lost
        r.stalled = true;
        break;
      }
      continue;
    }
    idle = 0;
    for (std::size_t i = 0; i < got; ++i) {
      if (batch[i] != r.received && r.first_out_of_order == UINT64_MAX) {
        r.first_out_of_order = r.received;
        r.first_bad_value = batch[i];
      }
      r.sum += batch[i];
      ++r.received;
    }
  }
  if (r.stalled) {
    // The producer may still be spinning on a full queue; drain it so the join can finish.
    uint64_t v;
    while (q.try_pop(v)) {
    }
  }
  producer.join();
  return r;
}

void expect_clean(const StressResult& r, uint64_t n, const char* what) {
  EXPECT_FALSE(r.stalled) << what << ": the consumer waited for an item that never came";
  EXPECT_EQ(r.received, n) << what;
  EXPECT_EQ(r.first_out_of_order, UINT64_MAX)
      << what << ": item " << r.first_out_of_order << " arrived as " << r.first_bad_value;
  EXPECT_EQ(r.sum, n * (n - 1) / 2) << what;  // every value exactly once
}

template <class Q, bool Batch = false>
void stress_case(std::size_t capacity, uint64_t n, const char* name) {
  const auto start = std::chrono::steady_clock::now();
  const StressResult r = run_stress<Q, Batch>(capacity, n);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "[spsc stress] " << name << " capacity " << capacity << ": " << n << " items in "
            << secs << " s\n";
  expect_clean(r, n, name);
}

TEST(SpscStress, DefaultVariantCapacity2ForcesConstantFullAndEmpty) {
  stress_case<SpscQueue<uint64_t>>(2, stress_n(), "cached+padded");
}
TEST(SpscStress, DefaultVariantCapacity64) {
  stress_case<SpscQueue<uint64_t>>(64, stress_n(), "cached+padded");
}
TEST(SpscStress, DefaultVariantCapacity1024) {
  stress_case<SpscQueue<uint64_t>>(1024, stress_n(), "cached+padded");
}

TEST(SpscStress, UncachedVariant) {
  stress_case<SpscQueue<uint64_t, false, true>>(2, stress_n() / 5, "uncached+padded");
  stress_case<SpscQueue<uint64_t, false, true>>(64, stress_n() / 5, "uncached+padded");
}
TEST(SpscStress, UnpaddedVariant) {
  stress_case<SpscQueue<uint64_t, true, false>>(2, stress_n() / 5, "cached+unpadded");
  stress_case<SpscQueue<uint64_t, true, false>>(64, stress_n() / 5, "cached+unpadded");
}
TEST(SpscStress, BatchPop) {
  stress_case<SpscQueue<uint64_t>, true>(64, stress_n() / 5, "batch pop");
  stress_case<SpscQueue<uint64_t>, true>(1024, stress_n() / 5, "batch pop");
}

// A slot is eight words that must all agree. If the consumer ever saw a slot that was only
// partly written, or one being overwritten, some word would differ from the others.
struct Wide {
  uint64_t w[8] = {};
};

TEST(SpscStress, WideSlotsArriveWholeNotTorn) {
  constexpr std::size_t kCapacity = 8;
  const uint64_t n = stress_n() / 5;
  SpscQueue<Wide> q(kCapacity);
  std::atomic<bool> go{false};
  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (uint64_t i = 0; i < n; ++i) {
      Wide item;
      for (uint64_t k = 0; k < 8; ++k) item.w[k] = i * 8 + k;
      while (!q.try_push(item)) cpu_relax();
    }
  });
  uint64_t received = 0;
  uint64_t torn = 0;
  uint64_t idle = 0;
  go.store(true, std::memory_order_release);
  while (received < n && idle < 400'000'000) {
    Wide item;
    if (!q.try_pop(item)) {
      cpu_relax();
      ++idle;
      continue;
    }
    idle = 0;
    for (uint64_t k = 0; k < 8; ++k) {
      if (item.w[k] != received * 8 + k) {
        ++torn;
        break;
      }
    }
    ++received;
  }
  Wide drain;
  while (q.try_pop(drain)) {
  }
  producer.join();
  EXPECT_EQ(received, n);
  EXPECT_EQ(torn, 0u);
}

}  // namespace
