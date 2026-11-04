add_executable(lob_tests smoke_test.cpp matcher_test.cpp fuzz_test.cpp)
target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)
