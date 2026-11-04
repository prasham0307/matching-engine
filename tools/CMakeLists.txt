add_executable(lob_demo demo.cpp)
target_link_libraries(lob_demo PRIVATE lob)

add_executable(lob_replay replay.cpp)
target_link_libraries(lob_replay PRIVATE lob)
