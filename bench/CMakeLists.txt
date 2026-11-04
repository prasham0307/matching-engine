add_executable(lob_bench smoke_bench.cpp)
target_link_libraries(lob_bench PRIVATE lob benchmark::benchmark_main)

add_executable(lob_latency latency_bench.cpp)
target_link_libraries(lob_latency PRIVATE lob)
target_compile_definitions(lob_latency PRIVATE LOB_BUILD_TYPE="$<CONFIG>")
