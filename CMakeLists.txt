# Enemy Territory with the statically linked `legacy` mod.
#
# Host builds (ET_TARGET=host) use the Unix platform layer with a no-op GL, so a
# client boots headless on x86 and on big-endian PPC under qemu-user.
# See docs/port-plan.md.
cmake_minimum_required(VERSION 3.12)
project(EnemyTerritory C CXX)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(ET_TARGET "host" CACHE STRING "host or mac")
set(ET_SRC ${CMAKE_SOURCE_DIR}/src)
set(ET_LEGACY ${CMAKE_SOURCE_DIR}/legacy)

include(cmake/EtSources.cmake)

set(ET_C_FLAGS -std=gnu99 -fgnu89-inline -fsigned-char -fcommon -fno-strict-aliasing)
set(ET_MOD_FLAGS -std=gnu11 -fgnu89-inline -fsigned-char -fno-common -fno-strict-aliasing)
set(ET_MOD_DEFS "")
if(ET_TARGET STREQUAL "mac")
    include(cmake/EtMacFlags.cmake)
endif()

if(ET_TARGET STREQUAL "host")
    add_compile_options(-g -O1 -fno-pie)
    add_link_options(-no-pie)
endif()

# ---- legacy modules, each global renamed by legacy/static/rename_<module>.h
function(et_module name)
    file(STRINGS ${ET_LEGACY}/static/${name}.sources list)
    file(STRINGS ${ET_LEGACY}/static/${name}.defines defs)
    file(STRINGS ${ET_LEGACY}/static/common.defines common)
    set(sources "")
    foreach(f ${list})
        list(APPEND sources ${ET_LEGACY}/${f})
    endforeach()
    add_library(${name}_mod STATIC ${sources})
    target_compile_definitions(${name}_mod PRIVATE ${defs} ${common} ${ET_MOD_DEFS} BUNDLED_CJSON ET_STATIC_MODULES)
    target_include_directories(${name}_mod PRIVATE ${ET_LEGACY}/static ${ET_LEGACY}/vendor/cjson ${ET_LEGACY}/src/qcommon/json)
    target_compile_options(${name}_mod PRIVATE ${ET_MOD_FLAGS} -w "SHELL:-include ${ET_LEGACY}/static/rename_${name}.h")
endfunction()

et_module(cgame)
et_module(ui)
et_module(qagame)

# ---- engine
if(ET_TARGET STREQUAL "host")
    include(cmake/EtHost.cmake)
else()
    include(cmake/EtMac.cmake)
endif()
