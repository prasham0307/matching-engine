add_executable(lob_tests smoke_test.cpp matcher_test.cpp fuzz_test.cpp order_pool_test.cpp id_index_test.cpp price_levels_test.cpp level_window_test.cpp flat_map_test.cpp matching_engine_test.cpp replay_test.cpp)
target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)

include(GoogleTest)
# A bug in the matching loop can make a test spin forever; fail it instead of hanging the run.
gtest_discover_tests(lob_tests PROPERTIES TIMEOUT 600)

# The golden replay input and expected trade log live in tests/data.
target_compile_definitions(lob_tests PRIVATE LOB_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")

# End-to-end test of the lob_replay executable (exit codes, golden trade log, determinism).
add_test(NAME ReplayCli
  COMMAND ${CMAKE_COMMAND}
    -DREPLAY=$<TARGET_FILE:lob_replay>
    -DDATA_DIR=${CMAKE_SOURCE_DIR}/tests/data
    -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/replay_cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/replay_cli_test.cmake)
