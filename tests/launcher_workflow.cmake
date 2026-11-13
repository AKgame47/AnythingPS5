# Headless launcher journey: package -> import -> pnr-launcher list/show/launch
# (text + JSON) -> history and honest compatibility, per-game jail, unknown ids.
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
message(STATUS "launcher workflow OK")
