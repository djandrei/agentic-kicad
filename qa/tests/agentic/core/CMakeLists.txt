add_executable( qa_agentic_core
    test_module.cpp
    test_version.cpp
    test_json_rpc.cpp
    test_mcp_server.cpp
    test_transports.cpp
    test_agent_host.cpp
    test_stage2.cpp
    test_stage3.cpp
)

target_include_directories( qa_agentic_core PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/qa/mocks/include
)

target_link_libraries( qa_agentic_core
    agentic_core
    common
    qa_utils
    ${wxWidgets_LIBRARIES}
    Boost::headers
    Boost::unit_test_framework
)

kicad_add_boost_test( qa_agentic_core qa_agentic_core )

setup_qa_env( qa_agentic_core )
