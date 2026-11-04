add_executable(lob_bench smoke_bench.cpp)
target_link_libraries(lob_bench PRIVATE lob benchmark::benchmark_main)
