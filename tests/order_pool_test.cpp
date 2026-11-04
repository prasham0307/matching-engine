#include "lob/order_pool.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <utility>
#include <vector>

namespace {

using lob::Order;
using lob::OrderPool;
using lob::Side;

Order make_order(uint64_t id) { return Order{id, 0, Side::Buy, static_cast<int64_t>(100 + id), 7, 7, id}; }

TEST(OrderPool, AllocateCopiesTheInitialValueAndClearsLinks) {
  OrderPool pool(4);
  Order* o = pool.allocate(make_order(42));
  EXPECT_EQ(o->id, 42u);
  EXPECT_EQ(o->price, 142);
  EXPECT_EQ(o->remaining, 7u);
  EXPECT_EQ(o->prev, nullptr);
  EXPECT_EQ(o->next, nullptr);
  EXPECT_EQ(pool.in_use(), 1u);
}

TEST(OrderPool, LiveOrdersHaveDistinctAddresses) {
  OrderPool pool(8);
  std::set<Order*> seen;
  for (uint64_t i = 0; i < 100; ++i) seen.insert(pool.allocate(make_order(i)));
  EXPECT_EQ(seen.size(), 100u);
}

TEST(OrderPool, AddressesAndContentsStayStableAcrossChunkGrowth) {
  OrderPool pool(4);
  std::vector<Order*> orders;
  for (uint64_t i = 0; i < 40; ++i) orders.push_back(pool.allocate(make_order(i)));

  EXPECT_EQ(pool.chunk_count(), 10u);
  EXPECT_EQ(pool.capacity(), 40u);
  for (uint64_t i = 0; i < orders.size(); ++i) {
    EXPECT_EQ(orders[i]->id, i);  // growing the pool never moved or overwrote an earlier order
  }
}

TEST(OrderPool, ReleasedSlotsAreReusedBeforeTheyGrowThePool) {
  OrderPool pool(4);
  std::vector<Order*> orders;
  for (uint64_t i = 0; i < 4; ++i) orders.push_back(pool.allocate(make_order(i)));
  ASSERT_EQ(pool.chunk_count(), 1u);

  pool.release(orders[1]);
  pool.release(orders[3]);
  EXPECT_EQ(pool.in_use(), 2u);

  Order* a = pool.allocate(make_order(10));
  Order* b = pool.allocate(make_order(11));
  EXPECT_EQ(pool.chunk_count(), 1u);  // no new chunk: both came from the free list
  EXPECT_EQ(std::set<Order*>({a, b}), std::set<Order*>({orders[1], orders[3]}));
  EXPECT_EQ(pool.in_use(), 4u);
}

TEST(OrderPool, SteadyStateChurnDoesNotGrowThePool) {
  OrderPool pool(16);
  std::vector<Order*> live;
  for (uint64_t i = 0; i < 100; ++i) live.push_back(pool.allocate(make_order(i)));
  const std::size_t chunks = pool.chunk_count();
  const std::size_t capacity = pool.capacity();

  for (uint64_t i = 0; i < 20000; ++i) {
    const std::size_t victim = i % live.size();
    pool.release(live[victim]);
    live[victim] = pool.allocate(make_order(1000 + i));
  }
  EXPECT_EQ(pool.chunk_count(), chunks);
  EXPECT_EQ(pool.capacity(), capacity);
  EXPECT_EQ(pool.in_use(), 100u);
}

TEST(OrderPool, MovedPoolKeepsOrdersValidAndLeavesSourceEmpty) {
  OrderPool a(4);
  std::vector<Order*> orders;
  for (uint64_t i = 0; i < 10; ++i) orders.push_back(a.allocate(make_order(i)));

  OrderPool b(std::move(a));
  EXPECT_EQ(b.in_use(), 10u);
  EXPECT_EQ(a.capacity(), 0u);  // NOLINT(bugprone-use-after-move): moved-from pool is defined empty
  EXPECT_EQ(a.in_use(), 0u);    // NOLINT(bugprone-use-after-move)
  for (uint64_t i = 0; i < orders.size(); ++i) EXPECT_EQ(orders[i]->id, i);

  OrderPool c;
  c = std::move(b);
  EXPECT_EQ(c.in_use(), 10u);
  for (uint64_t i = 0; i < orders.size(); ++i) EXPECT_EQ(orders[i]->id, i);
  c.release(orders[0]);
  EXPECT_EQ(c.in_use(), 9u);
}

}  // namespace
