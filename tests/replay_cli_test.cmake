# End-to-end test of the lob_replay executable. Run by ctest with:
#   -DREPLAY=<path to lob_replay> -DDATA_DIR=<tests/data> -DOUT_DIR=<scratch dir>
file(MAKE_DIRECTORY "${OUT_DIR}")

# 1. The golden input, verified against NaiveMatcher, must write exactly the golden trade log.
execute_process(
  COMMAND "${REPLAY}" "${DATA_DIR}/golden_events.csv" --verify --trades "${OUT_DIR}/golden_trades.out"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "golden replay exited with ${rc}\n${out}\n${err}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
  "${OUT_DIR}/golden_trades.out" "${DATA_DIR}/golden_trades.csv" RESULT_VARIABLE diff_rc)
if(NOT diff_rc EQUAL 0)
  message(FATAL_ERROR "trade log differs from tests/data/golden_trades.csv")
endif()
if(NOT out MATCHES "trade log hash \\(FNV-1a 64\\): 0x[0-9a-f]+")
  message(FATAL_ERROR "no hash line in the summary:\n${out}")
endif()

# 2. A malformed line must exit non-zero (2) and name the line number.
file(WRITE "${OUT_DIR}/bad.events" "LIMIT,1,10,BUY,100,5\n# comment\nLIMIT,2,10,BUY,oops,5\n")
execute_process(COMMAND "${REPLAY}" "${OUT_DIR}/bad.events"
  RESULT_VARIABLE bad_rc OUTPUT_VARIABLE bad_out ERROR_VARIABLE bad_err)
if(NOT bad_rc EQUAL 2)
  message(FATAL_ERROR "malformed input should exit 2, got ${bad_rc}")
endif()
if(NOT bad_err MATCHES "line 3:")
  message(FATAL_ERROR "error message does not name line 3:\n${bad_err}")
endif()

# 3. A missing input file must exit non-zero (3).
execute_process(COMMAND "${REPLAY}" "${OUT_DIR}/does_not_exist.events"
  RESULT_VARIABLE missing_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT missing_rc EQUAL 3)
  message(FATAL_ERROR "missing input file should exit 3, got ${missing_rc}")
endif()

# 4. Generate, then replay twice: the hashes must match.
execute_process(COMMAND "${REPLAY}" --generate "${OUT_DIR}/gen.events" --events 20000 --symbols 3 --seed 5
  RESULT_VARIABLE gen_rc OUTPUT_QUIET)
if(NOT gen_rc EQUAL 0)
  message(FATAL_ERROR "--generate failed with ${gen_rc}")
endif()
execute_process(COMMAND "${REPLAY}" "${OUT_DIR}/gen.events" RESULT_VARIABLE r1 OUTPUT_VARIABLE o1)
execute_process(COMMAND "${REPLAY}" "${OUT_DIR}/gen.events" RESULT_VARIABLE r2 OUTPUT_VARIABLE o2)
string(REGEX MATCH "0x[0-9a-f]+" h1 "${o1}")
string(REGEX MATCH "0x[0-9a-f]+" h2 "${o2}")
if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR h1 STREQUAL "" OR NOT h1 STREQUAL h2)
  message(FATAL_ERROR "two replays gave different hashes: '${h1}' vs '${h2}'")
endif()
