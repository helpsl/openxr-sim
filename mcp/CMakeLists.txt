# THE MCP SERVER (docs/DESIGN.md, "The MCP server"): the simulators driven by an agent, over stdio, through
# each simulator's control socket. Its own program: an MCP client starts it, and it outlives any one session.
if(WIN32)
    message(STATUS "openxr-sim: the MCP server is not ported to Windows yet")
    return()
endif()
include("${PROJECT_SOURCE_DIR}/cmake/jsoncpp.cmake")
oxs_notices_source(_oxs_notices)
add_executable(openxr-sim-mcp main.cpp "${_oxs_notices}")
target_include_directories(openxr-sim-mcp PRIVATE "${PROJECT_SOURCE_DIR}/common")
target_link_libraries(openxr-sim-mcp PRIVATE oxs::jsoncpp)
target_compile_definitions(openxr-sim-mcp PRIVATE OPENXR_SIM_VERSION="${PROJECT_VERSION}")
target_compile_options(openxr-sim-mcp PRIVATE ${OXS_WARNINGS})
install(TARGETS openxr-sim-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
