# THE MCP SERVER (docs/DESIGN.md, "The MCP server"): the simulators driven by an agent, over stdio, through
# each simulator's control socket. Its own program: an MCP client starts it, and it outlives any one session.
if(WIN32)
    message(STATUS "openxr-sim: the MCP server is not ported to Windows yet")
    return()
endif()
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
add_executable(openxr-sim-mcp main.cpp)
target_link_libraries(openxr-sim-mcp PRIVATE PkgConfig::JSONCPP)
target_compile_options(openxr-sim-mcp PRIVATE ${OXS_WARNINGS})
install(TARGETS openxr-sim-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
