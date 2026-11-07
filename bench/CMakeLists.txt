add_executable(lob_bench smoke_bench.cpp)
target_link_libraries(lob_bench PRIVATE lob benchmark::benchmark_main)

add_executable(lob_latency latency_bench.cpp)
target_link_libraries(lob_latency PRIVATE lob)
target_compile_definitions(lob_latency PRIVATE LOB_BUILD_TYPE="$<CONFIG>")

find_package(Threads REQUIRED)
add_executable(lob_pipeline pipeline_bench.cpp)
target_link_libraries(lob_pipeline PRIVATE lob Threads::Threads)
target_compile_definitions(lob_pipeline PRIVATE LOB_BUILD_TYPE="$<CONFIG>")

