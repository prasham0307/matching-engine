# matching-engine

A C++20 limit order book matching engine, built to learn production-style C++, performance measurement and market microstructure.

## Requirements

- g++ 13+ (or another C++20 compiler)
- CMake 3.20+
- Internet access on first configure (GoogleTest and Google Benchmark are fetched)

## Build and test

Debug (AddressSanitizer + UndefinedBehaviorSanitizer):

    cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-debug -j
    ctest --test-dir build-debug --output-on-failure

Release (-O3, the default build type):

    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release -j
    ctest --test-dir build-release --output-on-failure

## Benchmarks

    ./build-release/bench/lob_bench

## Interactive demo

    ./build-release/tools/lob_demo            # runs on OrderBook
    ./build-release/tools/lob_demo --naive    # same commands on NaiveMatcher

Type `help` for the commands. A good first session:

    scenario quiz
    limit buy 102 8

You can also pipe commands in: `./build-release/tools/lob_demo < commands.txt`.
