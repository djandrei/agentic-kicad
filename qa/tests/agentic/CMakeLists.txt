# Agentic-KiCad tests.  Added from the top-level CMakeLists.txt when KICAD_AGENTIC is ON.

add_subdirectory( core )

# End-to-end tests: a stdlib MCP client (qa/tests/agentic_e2e/client.py) drives the built
# kicad-cli and kicad over stdio and HTTP.  Tests that need a display skip without DISPLAY.
add_test( NAME qa_agentic_e2e
    COMMAND ${PYTHON_EXECUTABLE} -m pytest -v ${CMAKE_SOURCE_DIR}/qa/tests/agentic_e2e
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties( qa_agentic_e2e
    PROPERTIES
        ENVIRONMENT "KICAD_CLI=$<TARGET_FILE:kicad-cli>;KICAD_BIN=$<TARGET_FILE:kicad>"
)

setup_qa_env( qa_agentic_e2e )

# Timings on the largest demo board against doc/agentic/timings.json (stage 15).
add_test( NAME qa_agentic_perf
    COMMAND ${PYTHON_EXECUTABLE} -m pytest -v -s ${CMAKE_SOURCE_DIR}/qa/tests/agentic_perf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties( qa_agentic_perf
    PROPERTIES
        ENVIRONMENT "KICAD_CLI=$<TARGET_FILE:kicad-cli>;KICAD_BIN=$<TARGET_FILE:kicad>"
        TIMEOUT 1800
)

setup_qa_env( qa_agentic_perf )
