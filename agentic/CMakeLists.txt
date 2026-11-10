# Agentic-KiCad: MCP server, sessions and viewers.  See doc/agentic/upstream-merge.md.
#
# Fork code is included as <agentic/...> relative to the source root.  This directory is
# added after the kifaces and kicad/, and adds its sources to their targets, so their
# CMakeLists.txt files stay untouched.

set( AGENTIC_INCLUDE_ROOT ${CMAKE_SOURCE_DIR} )

find_package( Threads REQUIRED )

# KICAD_AGENTIC is defined only on the upstream files that hold a hook and on the fork's own
# targets.  Every other object file is then identical to the upstream build, so ccache shares
# them between build/upstream and build/agentic.  Each hooked file is listed in
# doc/agentic/upstream-merge.md.
set_property( SOURCE ${CMAKE_SOURCE_DIR}/kicad/kicad.cpp ${CMAKE_SOURCE_DIR}/kicad/kicad_cli.cpp
              DIRECTORY ${CMAKE_SOURCE_DIR}/kicad
              APPEND PROPERTY COMPILE_DEFINITIONS KICAD_AGENTIC )

set_property( SOURCE ${CMAKE_SOURCE_DIR}/pcbnew/pcbnew.cpp
              DIRECTORY ${CMAKE_SOURCE_DIR}/pcbnew
              APPEND PROPERTY COMPILE_DEFINITIONS KICAD_AGENTIC )

set_property( SOURCE ${CMAKE_SOURCE_DIR}/eeschema/eeschema.cpp
              DIRECTORY ${CMAKE_SOURCE_DIR}/eeschema
              APPEND PROPERTY COMPILE_DEFINITIONS KICAD_AGENTIC )

# Protocol and transports: no wx dependency.
add_library( agentic_protocol STATIC
    protocol/json_rpc.cpp
    protocol/mcp_server.cpp
    protocol/registry.cpp
    protocol/schema.cpp
    transport/http_transport.cpp
    transport/stdio_transport.cpp
)

target_include_directories( agentic_protocol PUBLIC ${AGENTIC_INCLUDE_ROOT} )
target_compile_definitions( agentic_protocol PRIVATE KICAD_AGENTIC )

target_link_libraries( agentic_protocol PUBLIC
    nlohmann_json
    Boost::headers
    Threads::Threads
)

# Core: agent host, sessions.  Linked into kicad and kicad-cli.
add_library( agentic_core STATIC
    core/agent_host.cpp
    core/agent_project.cpp
    core/agentic_version.cpp
    core/core_tools.cpp
    core/doc_session.cpp
    core/file_history.cpp
    core/fetch.cpp
    core/file_store.cpp
    core/handles.cpp
    core/library_tools.cpp
    core/main_queue.cpp
    core/message_log.cpp
    core/pcm_tools.cpp
    core/progress.cpp
    core/services.cpp
    core/session_manager.cpp
    docs/docs_index.cpp
    docs/docs_tools.cpp
)

target_include_directories( agentic_core PUBLIC
    ${AGENTIC_INCLUDE_ROOT}
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions( agentic_core PRIVATE KICAD_AGENTIC )

target_link_libraries( agentic_core PUBLIC
    agentic_protocol
    kicommon
    kiplatform
    picosha2
    ${wxWidgets_LIBRARIES}
)

# Hub and viewer shell: linked into kicad (viewer mode).
add_library( agentic_ui STATIC
    hub/agent_viewer_mode.cpp
    hub/hub_frame.cpp
    viewer_shell/agent_viewer.cpp
)

target_compile_definitions( agentic_ui PRIVATE KICAD_AGENTIC )

target_link_libraries( agentic_ui PUBLIC
    agentic_core
    ${wxWidgets_LIBRARIES}
)

# Hosts
target_sources( kicad-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/cli/command_mcp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cli/fd_streams.cpp
)

target_include_directories( kicad-cli PRIVATE ${AGENTIC_INCLUDE_ROOT} ${CMAKE_SOURCE_DIR}/kicad/cli )
target_link_libraries( kicad-cli agentic_core )

target_include_directories( kicad PRIVATE ${AGENTIC_INCLUDE_ROOT} )
target_link_libraries( kicad agentic_ui )

# Providers are compiled into their kifaces.  The fork's include root is given to the hooked
# file and the provider sources only, not to the whole kiface target, for the same ccache
# reason as KICAD_AGENTIC above.
set( AGENTIC_PCB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/board_doc_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/pcb_agent_provider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/pcb_check_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/pcb_items.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/pcb_library_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/pcb_route_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/pcb_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/pcb_viewer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcb/pns_agent_iface.cpp
)

set( AGENTIC_SCH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sch_agent_provider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sch_check_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sch_doc_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sch_items.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sch_library_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sch_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sch_viewer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sym_lib_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sym_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sch/sym_viewer.cpp
)

target_sources( pcbnew_kiface_objects PRIVATE ${AGENTIC_PCB_SOURCES} )
target_sources( eeschema_kiface_objects PRIVATE ${AGENTIC_SCH_SOURCES} )

set_property( SOURCE ${CMAKE_SOURCE_DIR}/pcbnew/pcbnew.cpp ${AGENTIC_PCB_SOURCES}
              DIRECTORY ${CMAKE_SOURCE_DIR}/pcbnew
              APPEND PROPERTY INCLUDE_DIRECTORIES ${AGENTIC_INCLUDE_ROOT} )

set_property( SOURCE ${CMAKE_SOURCE_DIR}/eeschema/eeschema.cpp ${AGENTIC_SCH_SOURCES}
              DIRECTORY ${CMAKE_SOURCE_DIR}/eeschema
              APPEND PROPERTY INCLUDE_DIRECTORIES ${AGENTIC_INCLUDE_ROOT} )

# The provider sources call the fork's core.  INTERFACE, so the kiface's own objects keep
# their upstream compile flags; the kiface modules and the tests that link the object
# libraries get agentic_core.
target_link_libraries( pcbnew_kiface_objects INTERFACE agentic_ui agentic_core )
target_link_libraries( eeschema_kiface_objects INTERFACE agentic_ui agentic_core )
