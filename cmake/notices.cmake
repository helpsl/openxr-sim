# THIRD_PARTY_NOTICES.md INSIDE THE PROGRAMS, for `--licenses`: the Windows .exe is the whole download, so
# the notices travel in it. Bytes, not a string literal (MSVC limits a literal's length). Read at configure
# time; editing the file reconfigures.
function(oxs_notices_source out)
    set(src "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
    file(READ "${src}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    # Sixteen to a line (CMake's expressions have no {16}).
    string(REPEAT "0x..," 16 _line)
    string(REGEX REPLACE "(${_line})" "\\1\n    " bytes "${bytes}")
    set(file "${CMAKE_BINARY_DIR}/oxs_notices.cpp")
    file(CONFIGURE OUTPUT "${file}" CONTENT
"// Generated from THIRD_PARTY_NOTICES.md (cmake/notices.cmake).
#include \"notices.hpp\"
namespace {
const unsigned char kBytes[] = {
    @bytes@0x00 };
}
namespace oxs {
extern const char* const kThirdPartyNotices = reinterpret_cast<const char*>(kBytes);
}
" @ONLY)
    set(${out} "${file}" PARENT_SCOPE)
endfunction()
