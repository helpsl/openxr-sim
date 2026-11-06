# openxr-sim: an OpenXR runtime whose headset is a window on the desktop. README.md says what it is,
# docs/DESIGN.md how it works.
cmake_minimum_required(VERSION 3.21)
project(openxr-sim VERSION 0.8.3 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
include(cmake/notices.cmake)
# Every program and the runtime in one directory (one per configuration with Visual Studio's generators),
# where the test manifest and tests/sim-test.sh look for them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

option(OPENXR_SIM_BUILD_SIMULATOR "the simulator's window (GLFW)" ON)
option(OPENXR_SIM_BUILD_MCP "the MCP server" ON)
option(OPENXR_SIM_BUILD_TESTS "the probe and the end-to-end test's client" ON)
# GLFW from third_party/glfw rather than the system's: the default on Windows, where there is no system's.
if(WIN32)
    set(_bundled_glfw ON)
else()
    set(_bundled_glfw OFF)
endif()
option(OPENXR_SIM_BUNDLED_GLFW "build GLFW from third_party/glfw" ${_bundled_glfw})
# A system default runtime (/etc/xdg/openxr/1/active_runtime.json) naming this one. Off: a distribution
# that wants it the default says so.
option(OPENXR_SIM_INSTALL_ACTIVE_RUNTIME "install active_runtime.json naming this runtime" OFF)

if(MSVC)
    set(OXS_WARNINGS /W4 /wd4100 /wd4201 /wd4204 /wd4221)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
else()
    set(OXS_WARNINGS -Wall -Wextra)
endif()

# ---- the headers: OpenXR's and Vulkan's -------------------------------------------------------------
# The runtime links neither loader: the OpenXR loader opens the runtime, and Vulkan's and GL's functions
# are looked up at run time in whatever the application has loaded.
find_package(PkgConfig)
if(PKG_CONFIG_FOUND AND NOT WIN32)
    pkg_check_modules(OPENXR openxr)
    pkg_check_modules(VULKAN vulkan)
endif()
set(OPENXR_SIM_OPENXR_INCLUDE "${OPENXR_INCLUDE_DIRS}" CACHE PATH "OpenXR SDK headers (the directory holding openxr/)")
set(OPENXR_SIM_VULKAN_INCLUDE "${VULKAN_INCLUDE_DIRS}" CACHE PATH "Vulkan headers (the directory holding vulkan/)")
if(NOT OPENXR_SIM_VULKAN_INCLUDE AND DEFINED ENV{VULKAN_SDK})
    set(OPENXR_SIM_VULKAN_INCLUDE "$ENV{VULKAN_SDK}/Include" CACHE PATH "" FORCE)
endif()

# ---- the runtime -------------------------------------------------------------------------------------
set(OXS_RUNTIME_SOURCES runtime/runtime.c runtime/session.c runtime/hand.c runtime/swapchain.c runtime/swapchain_vk.c
    runtime/swapchain_gl.c runtime/input.c runtime/device_null.c runtime/device_sim.c)
if(WIN32)
    list(APPEND OXS_RUNTIME_SOURCES runtime/plat_win32.c runtime/swapchain_d3d11.c runtime/swapchain_d3d12.c runtime/openxr_sim.def)
else()
    list(APPEND OXS_RUNTIME_SOURCES runtime/plat_posix.c)
endif()
# A MODULE, in a directory of its own: nothing links it, the loader opens it by the path in its manifest.
add_library(openxr_sim MODULE ${OXS_RUNTIME_SOURCES})
target_include_directories(openxr_sim PRIVATE runtime common ${OPENXR_SIM_OPENXR_INCLUDE} ${OPENXR_SIM_VULKAN_INCLUDE})
target_compile_definitions(openxr_sim PRIVATE XR_EXTENSION_PROTOTYPES OPENXR_SIM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    OPENXR_SIM_VERSION_MINOR=${PROJECT_VERSION_MINOR} OPENXR_SIM_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_compile_options(openxr_sim PRIVATE ${OXS_WARNINGS})
if(WIN32)
    set(OXS_LIBDIR "${CMAKE_INSTALL_BINDIR}")
    set_target_properties(openxr_sim PROPERTIES PREFIX "")
    # dxguid: the interface IDs, a static library -- d3d11.dll, d3d12.dll and dxgi.dll are looked up at run
    # time, as Vulkan's and GL's are, and loaded only by an application that uses them.
    target_link_libraries(openxr_sim PRIVATE dxguid)
    install(TARGETS openxr_sim LIBRARY DESTINATION "${OXS_LIBDIR}")
else()
    set(OXS_LIBDIR "${CMAKE_INSTALL_LIBDIR}/openxr-sim")
    target_compile_definitions(openxr_sim PRIVATE OPENXR_SIM_PATH="${CMAKE_INSTALL_FULL_BINDIR}/openxr-sim")
    target_compile_options(openxr_sim PRIVATE -Wno-cast-function-type -fvisibility=hidden)
    target_link_libraries(openxr_sim PRIVATE pthread ${CMAKE_DL_LIBS} m)
    # Every symbol resolved at link time: a module with an undefined one fails at dlopen, inside the
    # application, as "failed to load a runtime".
    target_link_options(openxr_sim PRIVATE -Wl,--no-undefined)
    set_target_properties(openxr_sim PROPERTIES PREFIX "lib")
    install(TARGETS openxr_sim LIBRARY DESTINATION "${OXS_LIBDIR}")
endif()

# THE MANIFESTS: the installed one, naming the library by its installed path (on Windows, beside the
# manifest); and one naming the library in the BUILD tree, so everything can run before it is installed:
#   XR_RUNTIME_JSON=build/test_runtime.json build/openxr-sim-probe
if(WIN32)
    set(OPENXR_SIM_MANIFEST_LIBRARY "./openxr_sim.dll")
    configure_file(runtime/openxr_sim.json.in "${CMAKE_CURRENT_BINARY_DIR}/openxr_sim.json" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openxr_sim.json" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/test_runtime.json" CONTENT
"{\n    \"file_format_version\": \"1.0.0\",\n    \"runtime\": { \"name\": \"openxr-sim\", \"library_path\": \"$<TARGET_FILE:openxr_sim>\" }\n}\n")
else()
    set(OPENXR_SIM_MANIFEST_LIBRARY "${CMAKE_INSTALL_FULL_LIBDIR}/openxr-sim/libopenxr_sim.so")
    configure_file(runtime/openxr_sim.json.in "${CMAKE_CURRENT_BINARY_DIR}/openxr_sim.json" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openxr_sim.json" DESTINATION "${CMAKE_INSTALL_DATADIR}/openxr/1")
    set(OPENXR_SIM_MANIFEST_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/libopenxr_sim.so")
    configure_file(runtime/openxr_sim.json.in "${CMAKE_CURRENT_BINARY_DIR}/test_runtime.json" @ONLY)
    if(OPENXR_SIM_INSTALL_ACTIVE_RUNTIME)
        set(OPENXR_SIM_MANIFEST_LIBRARY "${CMAKE_INSTALL_FULL_LIBDIR}/openxr-sim/libopenxr_sim.so")
        configure_file(runtime/openxr_sim.json.in "${CMAKE_CURRENT_BINARY_DIR}/active_runtime.json" @ONLY)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/active_runtime.json" DESTINATION /etc/xdg/openxr/1)
    endif()
endif()

# ---- the simulator -----------------------------------------------------------------------------------
if(OPENXR_SIM_BUILD_SIMULATOR)
    add_subdirectory(sim)
endif()

# ---- the MCP server ----------------------------------------------------------------------------------
if(OPENXR_SIM_BUILD_MCP)
    add_subdirectory(mcp)
endif()

# ---- tests -------------------------------------------------------------------------------------------
if(OPENXR_SIM_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---- a .deb (Linux) ----------------------------------------------------------------------------------
# `cpack` in a build configured with -DCMAKE_INSTALL_PREFIX=/usr: the paths the runtime and its manifest
# name are fixed when it is configured, so the package installs where the build was told it would be.
# The libraries it links are found by dpkg-shlibdeps; what GLFW opens at run time (Wayland's and X11's
# client libraries, xkbcommon) it cannot see, so those are named here. .github/workflows/build.yml builds
# it on Ubuntu 24.04 for each release.
if(NOT WIN32)
    # The licences: ours and the third parties' (THIRD_PARTY_NOTICES.md, which --licenses also prints).
    # Also both in one `copyright`, Debian's name for it: a minimized Ubuntu (its containers, its minimal
    # installs) keeps nothing else of /usr/share/doc.
    install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
    file(READ "${PROJECT_SOURCE_DIR}/LICENSE" _oxs_license)
    file(READ "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" _oxs_notices)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/copyright"
        "openxr-sim -- https://github.com/helpsl/openxr-sim\n\nopenxr-sim is licensed under the Apache License, Version 2.0:\n\n"
        "${_oxs_license}\n\n${_oxs_notices}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/copyright" DESTINATION "${CMAKE_INSTALL_DOCDIR}")
    if(OPENXR_SIM_BUILD_SIMULATOR)
        install(FILES packaging/openxr-sim.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    endif()
    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGE_NAME openxr-sim)
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_CONTACT "Lawrence Helps <lawrence@helpsl.net>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OpenXR runtime with a simulated headset in a window")
    # CPack writes the control file's Description from these: the summary as its first line, then this.
    set(CPACK_PACKAGE_DESCRIPTION "An OpenXR application runs as it would on a headset; its two eyes appear side by side on \
the desktop, and the headset and both controllers are moved with the mouse and keyboard, a control socket, or an MCP \
server (openxr-sim-mcp). Without the simulated headset switched on it reports no headset connected, so applications \
with an optional VR mode start normally. To use it, run openxr-sim and choose Make this the active runtime.")
    set(CPACK_DEBIAN_PACKAGE_SECTION utils)
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/helpsl/openxr-sim")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libxkbcommon0, libwayland-cursor0, libwayland-egl1")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libopenxr-loader1, libdecor-0-0, libx11-6, libxrandr2, libxinerama1, libxcursor1, libxi6")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
    set(CPACK_STRIP_FILES ON)
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    include(CPack)
endif()
