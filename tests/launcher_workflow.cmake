# Headless launcher journey: package -> import -> pnr-launcher list/show/launch
# (text + JSON) -> history and honest compatibility, per-game jail, unknown ids.
# Start from a clean library so the test is repeatable in the same build dir.
file(REMOVE_RECURSE ${CMAKE_CURRENT_BINARY_DIR}/pnr_data ${CMAKE_CURRENT_BINARY_DIR}/scan_dir)
execute_process(COMMAND ${MAKE} ${CMAKE_CURRENT_BINARY_DIR}/launcher_demo.pnrgame
    launcher-demo "Launcher Demo" 0.2.0 bin/game bin/game=${PS_DEMO}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "make_package failed: ${out} ${err}")
endif()
execute_process(COMMAND ${PNR} import ${CMAKE_CURRENT_BINARY_DIR}/launcher_demo.pnrgame
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "IMPORT OK")
  message(FATAL_ERROR "import failed: ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} show launcher-demo
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "compatibility: Untested" OR NOT out MATCHES "launches:      0")
  message(FATAL_ERROR "show before launch wrong: ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} launch launcher-demo --json
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "\"exit_code\":0" OR NOT out MATCHES "\"recorded\":true" OR NOT out MATCHES "\"compat_advanced\":true")
  message(FATAL_ERROR "launch failed: rc=${rc} ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} list --json
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "\"game_id\":\"launcher-demo\"[^}]*\"compatibility\":\"Launches Only\"" OR NOT out MATCHES "\"launch_count\":1[,}]")
  message(FATAL_ERROR "list after launch wrong: ${out}")
endif()
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/pnr_data/jail/launcher-demo/psdemo_proof.txt)
  message(FATAL_ERROR "per-game jail proof file missing")
endif()
execute_process(COMMAND ${LAUNCHER} launch bogus-id
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT out MATCHES "NOT STARTED")
  message(FATAL_ERROR "bogus id did not fail honestly: rc=${rc} ${out}")
endif()
# ---- library management: human compatibility, history, read-only scan
execute_process(COMMAND ${LAUNCHER} compat set launcher-demo "Playable With Issues" --note "menu works, audio missing"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "COMPAT SET")
  message(FATAL_ERROR "compat set failed: ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} compat set launcher-demo "Fully Awesome"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "unknown compatibility state")
  message(FATAL_ERROR "bad state was not refused: rc=${rc} ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} compat set bogus-id Untested
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "unknown id on compat set should exit 2, got ${rc}")
endif()
execute_process(COMMAND ${LAUNCHER} launch launcher-demo --json
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "\"compat_advanced\":false")
  message(FATAL_ERROR "launch after compat set misbehaved: ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} compat show launcher-demo
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "Playable With Issues" OR NOT out MATCHES "set by a person" OR NOT out MATCHES "audio missing")
  message(FATAL_ERROR "human state was lost after a launch: ${out}")
endif()
# Re-importing the same package must not reset a person's state.
execute_process(COMMAND ${PNR} import ${CMAKE_CURRENT_BINARY_DIR}/launcher_demo.pnrgame
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
execute_process(COMMAND ${LAUNCHER} compat show launcher-demo
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "Playable With Issues")
  message(FATAL_ERROR "re-import reset a human-set state: ${out}")
endif()
execute_process(COMMAND ${LAUNCHER} history launcher-demo --json
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "\"launch_count\":2" OR NOT out MATCHES "\"exit_code\":0")
  message(FATAL_ERROR "history wrong: ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} history launcher-demo --limit 0
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0)
  message(FATAL_ERROR "history --limit 0 should be refused")
endif()
# `pnr run <id>` honors the human mark too, even for a human-set Untested.
execute_process(COMMAND ${LAUNCHER} compat set launcher-demo Untested --note "re-testing"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
execute_process(COMMAND ${PNR} run launcher-demo
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "compatibility=Untested .unchanged.")
  message(FATAL_ERROR "pnr run overwrote a human-set Untested: rc=${rc} ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} compat show launcher-demo
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "Untested" OR NOT out MATCHES "set by a person")
  message(FATAL_ERROR "human Untested lost after pnr run: ${out}")
endif()
# scan the directory holding the package just built: read-only
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/scan_dir)
file(COPY ${CMAKE_CURRENT_BINARY_DIR}/launcher_demo.pnrgame DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/scan_dir)
file(COPY ${PS_DEMO} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/scan_dir)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/scan_dir/readme.txt "not a game\n")
execute_process(COMMAND ${LAUNCHER} scan ${CMAKE_CURRENT_BINARY_DIR}/scan_dir
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "launcher_demo.pnrgame  \\[pnrgame\\]  valid package id=launcher-demo" OR NOT out MATCHES "already in library"
   OR NOT out MATCHES "valid x86-64 ELF" OR NOT out MATCHES "2 inspected, 1 other files skipped")
  message(FATAL_ERROR "scan output wrong: rc=${rc} ${out} ${err}")
endif()
execute_process(COMMAND ${LAUNCHER} scan ${CMAKE_CURRENT_BINARY_DIR}/no_such_dir
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0)
  message(FATAL_ERROR "scan of a missing directory should fail")
endif()
message(STATUS "launcher workflow OK")
