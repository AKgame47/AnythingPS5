# End-to-end PS-style game journey:
# package a sce-only binary, import it, run it BY GAME ID, and check the
# library bookkeeping (launch recorded, compatibility advances honestly).
execute_process(COMMAND ${MAKE} ${CMAKE_CURRENT_BINARY_DIR}/psdemo.pnrgame
    ps-demo "PNR PS Demo" 0.1.0 bin/game bin/game=${PS_DEMO}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "make_package failed: ${out} ${err}")
endif()

execute_process(COMMAND ${PNR} import ${CMAKE_CURRENT_BINARY_DIR}/psdemo.pnrgame
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "IMPORT OK")
  message(FATAL_ERROR "import failed: rc=${rc} ${out} ${err}")
endif()

# The whole point: run by id, no path. Every sce import must classify
# prx-implemented and the run must succeed.
execute_process(COMMAND ${PNR} run ps-demo
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "RUN OK")
  message(FATAL_ERROR "run-by-id failed: rc=${rc} ${out} ${err}")
endif()
if(NOT out MATCHES "14 prx-implemented, 0 prx-declared-only, 0 unknown")
  message(FATAL_ERROR "import classification off: ${out}")
endif()
if(NOT out MATCHES "compatibility=Launches Only")
  message(FATAL_ERROR "compatibility did not advance: ${out}")
endif()

# The game's own jail must hold its proof file.
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/pnr_data/jail/ps-demo/psdemo_proof.txt)
  message(FATAL_ERROR "per-game jail proof file missing")
endif()

# Library listing shows the new state.
execute_process(COMMAND ${PNR} games
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "Launches Only")
  message(FATAL_ERROR "library listing failed: ${out} ${err}")
endif()

# Unknown ids fail honestly.
execute_process(COMMAND ${PNR} run bogus-game-id
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "neither a file nor an installed game id")
  message(FATAL_ERROR "bogus id did not fail honestly: rc=${rc} ${out} ${err}")
endif()
message(STATUS "psgame workflow OK")
