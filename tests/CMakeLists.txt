# THE PROBE: what an application sees, through the real loader.
if(PKG_CONFIG_FOUND)
    pkg_check_modules(OPENXR_LOADER openxr)
    pkg_check_modules(VULKAN_LOADER vulkan)
    pkg_check_modules(EGL egl)
endif()
find_package(OpenXR CONFIG QUIET)   # the OpenXR SDK's own package, on Windows
if(OpenXR_FOUND)
    set(OXS_LOADER OpenXR::openxr_loader)
else()
    set(OXS_LOADER ${OPENXR_LOADER_LINK_LIBRARIES})
endif()
if(NOT OXS_LOADER)
    message(STATUS "openxr-sim: no OpenXR loader found; the probe and the test client are not built")
    return()
endif()
add_executable(openxr-sim-probe probe.c)
target_include_directories(openxr-sim-probe PRIVATE ${OPENXR_SIM_OPENXR_INCLUDE})
target_link_libraries(openxr-sim-probe PRIVATE ${OXS_LOADER})
install(TARGETS openxr-sim-probe RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/openxr-sim)

# THE END-TO-END TEST's client: a session on lavapipe or any Vulkan device, two coloured eyes, the
# simulator's scripted move read back (sim_client.c, run by sim-test.sh). Linux: it needs EGL for its GL runs.
if(NOT WIN32)
    add_executable(openxr-sim-client sim_client.c)
    target_include_directories(openxr-sim-client PRIVATE ${OPENXR_SIM_OPENXR_INCLUDE} ${OPENXR_SIM_VULKAN_INCLUDE})
    target_link_libraries(openxr-sim-client PRIVATE ${OXS_LOADER} ${VULKAN_LOADER_LINK_LIBRARIES} ${EGL_LINK_LIBRARIES} m)
endif()
