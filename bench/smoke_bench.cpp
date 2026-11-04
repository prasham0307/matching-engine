#include <benchmark/benchmark.h>

#include <cstdint>

static void BM_Smoke(benchmark::State& state) {
  std::int64_t x = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(x += 1);
  }
}
BENCHMARK(BM_Smoke);
