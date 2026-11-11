# Agentic-KiCad stage 0.2 spikes: throwaway feasibility programs (PLAN.md stage 0.2), kept for reference.
# Results are recorded in doc/agentic/spikes.md.

# S0.c: save and reload cost, board.
add_executable( spike_save_pcb
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp
    spike_init.cpp
    spike_save_pcb.cpp
)

target_compile_definitions( spike_save_pcb PRIVATE PCBNEW )
add_dependencies( spike_save_pcb pcbnew )

target_include_directories( spike_save_pcb PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/pcbnew
    ${CMAKE_SOURCE_DIR}/qa/mocks/include
    ${INC_AFTER}
)

target_link_libraries( spike_save_pcb
    qa_pcbnew_utils
    pcbnew_kiface_objects
    3d-viewer
    connectivity
    pcbcommon
    pnsrouter
    gal
    common
    gal
    qa_utils
    dxflib_qcad
    tinyspline_lib
    nanosvg
    planegcs
    markdown_lib
    ${PCBNEW_IO_LIBRARIES}
    ${wxWidgets_LIBRARIES}
    ${GDI_PLUS_LIBRARIES}
    Boost::headers
    ${PCBNEW_EXTRA_LIBS}
)

# S0.c: save and reload cost, schematic.
add_executable( spike_save_sch
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp
    spike_init.cpp
    spike_save_sch.cpp
)

target_compile_definitions( spike_save_sch PRIVATE EESCHEMA )
add_dependencies( spike_save_sch eeschema_kiface_objects )

target_include_directories( spike_save_sch PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/eeschema
    ${CMAKE_SOURCE_DIR}/qa/mocks/include
    ${INC_AFTER}
)

target_link_libraries( spike_save_sch
    eeschema_kiface_objects
    common
    kicommon
    pads_common
    kimath
    qa_utils
    qa_schematic_utils
    markdown_lib
    ${wxWidgets_LIBRARIES}
    ${GDI_PLUS_LIBRARIES}
    Boost::headers
)

# S0.e: canvas on a session, board and schematic.  Same libraries as the S0.c programs.
add_executable( spike_canvas_pcb
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp
    spike_init.cpp
    spike_canvas.cpp
    spike_canvas_pcb.cpp
)

target_compile_definitions( spike_canvas_pcb PRIVATE PCBNEW )
add_dependencies( spike_canvas_pcb pcbnew )
target_include_directories( spike_canvas_pcb PRIVATE $<TARGET_PROPERTY:spike_save_pcb,INCLUDE_DIRECTORIES> )
target_link_libraries( spike_canvas_pcb $<TARGET_PROPERTY:spike_save_pcb,LINK_LIBRARIES> )

add_executable( spike_canvas_sch
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp
    spike_init.cpp
    spike_canvas.cpp
    spike_canvas_sch.cpp
)

target_compile_definitions( spike_canvas_sch PRIVATE EESCHEMA )
add_dependencies( spike_canvas_sch eeschema_kiface_objects )
target_include_directories( spike_canvas_sch PRIVATE $<TARGET_PROPERTY:spike_save_sch,INCLUDE_DIRECTORIES> )
target_link_libraries( spike_canvas_sch $<TARGET_PROPERTY:spike_save_sch,LINK_LIBRARIES> )

# S0.a: frameless schematic edit.
add_executable( spike_frameless_sch
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp
    spike_init.cpp
    spike_frameless_sch.cpp
)

target_compile_definitions( spike_frameless_sch PRIVATE EESCHEMA )
add_dependencies( spike_frameless_sch eeschema_kiface_objects )
target_include_directories( spike_frameless_sch PRIVATE $<TARGET_PROPERTY:spike_save_sch,INCLUDE_DIRECTORIES> )
target_link_libraries( spike_frameless_sch $<TARGET_PROPERTY:spike_save_sch,LINK_LIBRARIES> )

# S0.b: frameless board edit, zone fill, DRC.
add_executable( spike_frameless_pcb
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp
    spike_init.cpp
    spike_frameless_pcb.cpp
)

target_compile_definitions( spike_frameless_pcb PRIVATE PCBNEW )
add_dependencies( spike_frameless_pcb pcbnew )
target_include_directories( spike_frameless_pcb PRIVATE $<TARGET_PROPERTY:spike_save_pcb,INCLUDE_DIRECTORIES> )
target_link_libraries( spike_frameless_pcb $<TARGET_PROPERTY:spike_save_pcb,LINK_LIBRARIES> )

# S0.d: headless PNS router.
add_executable( spike_pns
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp
    spike_init.cpp
    spike_pns.cpp
)

target_compile_definitions( spike_pns PRIVATE PCBNEW )
add_dependencies( spike_pns pcbnew )
target_include_directories( spike_pns PRIVATE $<TARGET_PROPERTY:spike_save_pcb,INCLUDE_DIRECTORIES> )
target_link_libraries( spike_pns $<TARGET_PROPERTY:spike_save_pcb,LINK_LIBRARIES> )

# S0.g: simulator in-process.
add_executable( spike_sim
    ${CMAKE_SOURCE_DIR}/qa/mocks/kicad/common_mocks.cpp
    spike_init.cpp
    spike_sim.cpp
)

target_compile_definitions( spike_sim PRIVATE EESCHEMA )
add_dependencies( spike_sim eeschema_kiface_objects )
target_include_directories( spike_sim PRIVATE $<TARGET_PROPERTY:spike_save_sch,INCLUDE_DIRECTORIES> )
target_link_libraries( spike_sim $<TARGET_PROPERTY:spike_save_sch,LINK_LIBRARIES> ${NGSPICE_LIBRARY} )
