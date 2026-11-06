# Build manifest, from the Quake III port. After MakePEF, the build writes
# <name>.manifest.txt next to <name>.pef with:
#   pef_sha256         the PEF's SHA-256;
#   gcc_version        the first line of the C compiler's --version;
#   retro68_commit     the commit of the Retro68 source next to the toolchain
#                      (tools/Retro68-src for tools/Retro68-build), "unknown"
#                      when there is no git checkout there;
#   retro68_submodule  "<path> <commit>" for each of its submodules;
#   retro68_pinned     "yes" when commit and submodules are the ones
#                      retro68-versions.txt pins;
#   static_modules_ldscript_sha256
#                      the SHA-256 of the linker script that brackets the
#                      static game modules, generated from the
#                      toolchain's default script.
# The manifest holds no dates or paths, so it changes only with the PEF or the
# toolchain. The PEF itself embeds the source directory (assert() messages
# carry __FILE__), so the same source built in another directory has another
# pef_sha256.
#
# cmake/EtMac.cmake includes this file and calls et_add_build_manifest(); the
# build runs this file again in script mode (cmake -P) to write the manifest.

if(NOT CMAKE_SCRIPT_MODE_FILE)
    find_package(Git QUIET)
    set(ET_BUILD_MANIFEST_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

    # et_add_build_manifest(<name>): <name>.manifest.txt from <name>.pef,
    # built with CMAKE_C_COMPILER from tools/Retro68-build.
    function(et_add_build_manifest name)
        set(retro68_tools "${CMAKE_SOURCE_DIR}/tools")
        set(pef "${CMAKE_CURRENT_BINARY_DIR}/${name}.pef")
        set(manifest "${CMAKE_CURRENT_BINARY_DIR}/${name}.manifest.txt")
        set(versions "${CMAKE_SOURCE_DIR}/retro68-versions.txt")
        set(ldscript "${ET_LDSCRIPT}")
        add_custom_command(
            OUTPUT "${name}.manifest.txt"
            COMMAND "${CMAKE_COMMAND}"
                    "-DET_MANIFEST_PEF=${pef}"
                    "-DET_MANIFEST_OUTPUT=${manifest}"
                    "-DET_MANIFEST_COMPILER=${CMAKE_C_COMPILER}"
                    "-DET_MANIFEST_GIT=${GIT_EXECUTABLE}"
                    "-DET_MANIFEST_RETRO68_SOURCE=${retro68_tools}/Retro68-src"
                    "-DET_MANIFEST_VERSIONS=${versions}"
                    "-DET_MANIFEST_LDSCRIPT=${ldscript}"
                    -P "${ET_BUILD_MANIFEST_SCRIPT}"
            DEPENDS "${name}.pef" "${versions}" "${ET_BUILD_MANIFEST_SCRIPT}" ${ldscript}
            COMMENT "Recording the build manifest of ${name}"
            VERBATIM)
    endfunction()
    return()
endif()

cmake_minimum_required(VERSION 3.12)
foreach(variable ET_MANIFEST_PEF ET_MANIFEST_OUTPUT ET_MANIFEST_COMPILER
                 ET_MANIFEST_VERSIONS)
    if(NOT ${variable})
        message(FATAL_ERROR "${variable} is not set.")
    endif()
endforeach()

file(SHA256 "${ET_MANIFEST_PEF}" pef_sha256)
get_filename_component(pef_name "${ET_MANIFEST_PEF}" NAME)

execute_process(COMMAND "${ET_MANIFEST_COMPILER}" --version
    RESULT_VARIABLE status OUTPUT_VARIABLE gcc_version ERROR_VARIABLE gcc_error)
string(REGEX MATCH "^[^\r\n]*" gcc_version "${gcc_version}")
if(NOT status EQUAL 0 OR gcc_version STREQUAL "")
    message(FATAL_ERROR "${ET_MANIFEST_COMPILER} --version failed (${status}): ${gcc_error}")
endif()

# Read the toolchain's source without writing to it (no index refresh).
set(ENV{GIT_OPTIONAL_LOCKS} 0)
set(commit "unknown")
set(submodules "")
if(ET_MANIFEST_GIT AND EXISTS "${ET_MANIFEST_RETRO68_SOURCE}/.git")
    execute_process(
        COMMAND "${ET_MANIFEST_GIT}" -C "${ET_MANIFEST_RETRO68_SOURCE}" rev-parse --verify HEAD
        RESULT_VARIABLE head_status OUTPUT_VARIABLE head
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND "${ET_MANIFEST_GIT}" -C "${ET_MANIFEST_RETRO68_SOURCE}" submodule status --recursive
        RESULT_VARIABLE submodule_status OUTPUT_VARIABLE submodule_lines ERROR_QUIET)
    if(head_status EQUAL 0 AND submodule_status EQUAL 0 AND head MATCHES "^[0-9a-f]+$")
        set(commit "${head}")
        # "<flag><commit> <path> (<describe>)"; the flag is a space when the
        # submodule is checked out at the commit Retro68 records.
        string(REPLACE "\n" ";" submodule_lines "${submodule_lines}")
        foreach(line IN LISTS submodule_lines)
            if(line MATCHES "^(.)([0-9a-f]+) ([^ ]+)")
                if(CMAKE_MATCH_1 STREQUAL " ")
                    list(APPEND submodules "${CMAKE_MATCH_3} ${CMAKE_MATCH_2}")
                else()
                    list(APPEND submodules
                        "${CMAKE_MATCH_3} ${CMAKE_MATCH_2} (git submodule status flag '${CMAKE_MATCH_1}')")
                endif()
            endif()
        endforeach()
        list(SORT submodules)
    endif()
endif()

file(STRINGS "${ET_MANIFEST_VERSIONS}" pinned_commit REGEX "^RETRO68_COMMIT=")
string(REPLACE "RETRO68_COMMIT=" "" pinned_commit "${pinned_commit}")
string(STRIP "${pinned_commit}" pinned_commit)
file(STRINGS "${ET_MANIFEST_VERSIONS}" pin_lines REGEX "^RETRO68_SUBMODULE=")
set(pinned_submodules "")
foreach(line IN LISTS pin_lines)
    string(REPLACE "RETRO68_SUBMODULE=" "" line "${line}")
    string(STRIP "${line}" line)
    list(APPEND pinned_submodules "${line}")
endforeach()
list(SORT pinned_submodules)
if(NOT pinned_commit MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "${ET_MANIFEST_VERSIONS} does not pin RETRO68_COMMIT.")
endif()
if(commit STREQUAL pinned_commit AND submodules STREQUAL pinned_submodules)
    set(pinned "yes")
else()
    set(pinned "no (retro68-versions.txt pins ${pinned_commit})")
endif()

set(text "# ${pef_name} build manifest, written by the build after MakePEF.\n")
string(APPEND text "# retro68_* describe the Retro68 source next to the toolchain;\n")
string(APPEND text "# see retro68-versions.txt and docs/building-mac-os9.md.\n")
string(APPEND text "pef=${pef_name}\n")
string(APPEND text "pef_sha256=${pef_sha256}\n")
string(APPEND text "gcc_version=${gcc_version}\n")
string(APPEND text "retro68_commit=${commit}\n")
foreach(submodule IN LISTS submodules)
    string(APPEND text "retro68_submodule=${submodule}\n")
endforeach()
string(APPEND text "retro68_pinned=${pinned}\n")
if(ET_MANIFEST_LDSCRIPT)
    file(SHA256 "${ET_MANIFEST_LDSCRIPT}" ldscript_sha256)
    string(APPEND text "static_modules_ldscript_sha256=${ldscript_sha256}\n")
endif()
file(WRITE "${ET_MANIFEST_OUTPUT}" "${text}")
