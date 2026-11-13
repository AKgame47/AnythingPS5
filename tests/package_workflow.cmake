# End-to-end package workflow test:
# build a .pnrgame from the demo app, import it via the CLI, list the library.
# Fails the test if any step fails or output is missing.
execute_process(COMMAND ${MAKE} ${CMAKE_CURRENT_BINARY_DIR}/demo.pnrgame
    demo-game "PNR Demo Game" 0.1.0 bin/game bin/game=${EXE}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "make_package failed: ${out} ${err}")
endif()

execute_process(COMMAND ${PNR} import ${CMAKE_CURRENT_BINARY_DIR}/demo.pnrgame
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "IMPORT OK")
  message(FATAL_ERROR "import failed: rc=${rc} ${out} ${err}")
endif()

execute_process(COMMAND ${PNR} games
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "PNR Demo Game")
  message(FATAL_ERROR "library listing failed: ${out} ${err}")
endif()

# Imported game must run natively and exit 0.
execute_process(COMMAND ${PNR} run
    ${CMAKE_CURRENT_BINARY_DIR}/pnr_data/games/demo-game-0.1.0/bin/game
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "RUN OK")
  message(FATAL_ERROR "imported game run failed: rc=${rc} ${out} ${err}")
endif()
message(STATUS "package workflow OK")
