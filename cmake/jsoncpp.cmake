# JSONCPP FOR THE MCP SERVER, as the target oxs::jsoncpp: the system's (pkg-config), or -- with
# OPENXR_SIM_BUNDLED_JSONCPP -- JsonCpp's official release, fetched at configure time and compiled into the
# server statically.
#
# Why bundled exists: a program linked to the system's JsonCpp records its library's versioned name
# (libjsoncpp.so.25 on Ubuntu 24.04, .so.26 on Debian 13 and Ubuntu 26.04), and starts only where that one
# is installed -- JsonCpp changes the name when its C++ interface changes. Static, the .deb depends only on
# libraries whose names have not changed in years, so one built on the oldest system installs on the
# newer ones (docs/NOTES.md, "Releases on GitHub, and a .deb").
#
# The tarball is pinned by its SHA-256, and only DOWNLOADED: its own CMake project is not added
# (SOURCE_SUBDIR names a directory it does not have), so none of its options, tests or install rules
# reach this project -- its three sources are built here, as our target. An offline build can point
# FETCHCONTENT_SOURCE_DIR_OXS_JSONCPP at an unpacked copy. Off by default: a distribution, and LOS's forge
# (no network in its sandbox), link the system's.
option(OPENXR_SIM_BUNDLED_JSONCPP "fetch JsonCpp's release and link it statically into the MCP server" OFF)

if(OPENXR_SIM_BUNDLED_JSONCPP)
    include(FetchContent)
    FetchContent_Declare(oxs_jsoncpp
        URL https://github.com/open-source-parsers/jsoncpp/archive/refs/tags/1.9.8.tar.gz
        URL_HASH SHA256=51828cf3574281d2b79ec2a1c56a9e4c20cc1103711321ea96384cffb8d2d904
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR not-its-cmake-project)
    FetchContent_MakeAvailable(oxs_jsoncpp)
    set(_jsoncpp "${oxs_jsoncpp_SOURCE_DIR}")
    add_library(oxs_jsoncpp STATIC
        "${_jsoncpp}/src/lib_json/json_reader.cpp"
        "${_jsoncpp}/src/lib_json/json_value.cpp"
        "${_jsoncpp}/src/lib_json/json_writer.cpp")
    target_include_directories(oxs_jsoncpp SYSTEM PUBLIC "${_jsoncpp}/include")
    target_include_directories(oxs_jsoncpp PRIVATE "${_jsoncpp}/src/lib_json")
    # Position-independent: it goes into a PIE. Hidden: nothing of it is exported from the server.
    set_target_properties(oxs_jsoncpp PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
    # Not ours to warn about.
    target_compile_options(oxs_jsoncpp PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
    add_library(oxs::jsoncpp ALIAS oxs_jsoncpp)
    message(STATUS "openxr-sim: JsonCpp 1.9.8, fetched and linked statically")
else()
    pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
    add_library(oxs_jsoncpp INTERFACE)
    target_link_libraries(oxs_jsoncpp INTERFACE PkgConfig::JSONCPP)
    add_library(oxs::jsoncpp ALIAS oxs_jsoncpp)
endif()
