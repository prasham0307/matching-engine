add_executable(lob_tests smoke_test.cpp matcher_test.cpp fuzz_test.cpp order_pool_test.cpp id_index_test.cpp price_levels_test.cpp level_window_test.cpp)
target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)

include(GoogleTest)
# A bug in the matching loop can make a test spin forever; fail it instead of hanging the run.
gtest_discover_tests(lob_tests PROPERTIES TIMEOUT 600)
