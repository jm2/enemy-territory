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

# Host only: the whole engine and the static modules under sanitizers, e.g.
#   -DET_SANITIZE=address,undefined,float-cast-overflow [-DET_SANITIZE_STRICT=ON]
# ET_SANITIZE_LIBDIR links the runtimes statically from a directory holding
# libasan.a and libubsan.a, for an -m32 build where the i686 runtimes are not
# installed. See tests/compat/README.md.
set(ET_SANITIZE "" CACHE STRING "host only: -fsanitize= list, empty for none")
option(ET_SANITIZE_STRICT "host only: stop at the first report, UBSan included" OFF)
set(ET_SANITIZE_LIBDIR "" CACHE PATH "host only: directory with static sanitizer runtimes")
if(ET_SANITIZE)
    if(NOT ET_TARGET STREQUAL "host")
        message(FATAL_ERROR "ET_SANITIZE is for ET_TARGET=host only")
    endif()
    set(et_san -fsanitize=${ET_SANITIZE} -fno-omit-frame-pointer)
    if(ET_SANITIZE_STRICT)
        list(APPEND et_san -fno-sanitize-recover=undefined,float-cast-overflow)
    elseif(ET_SANITIZE MATCHES "address")
        # lets ASAN_OPTIONS=halt_on_error=0 go on past a report
        list(APPEND et_san -fsanitize-recover=address)
    endif()
    add_compile_options(${et_san})
    add_link_options(${et_san})
    # libjpeg's Huffman reader shifts its bit buffer through the sign bit by
    # design, as tests/security/run.sh also allows
    set_source_files_properties(${ET_JPEG} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=shift;-fno-sanitize=signed-integer-overflow")
    if(ET_SANITIZE_LIBDIR)
        add_link_options(-L${ET_SANITIZE_LIBDIR})
        if(ET_SANITIZE MATCHES "address")
            add_link_options(-static-libasan)
        endif()
        if(ET_SANITIZE MATCHES "undefined|float-cast-overflow")
            add_link_options(-static-libubsan)
        endif()
    endif()
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
    if(ET_SANITIZE AND ET_SANITIZE_STRICT)
        # legacy is kept as released: its UBSan reports stay recoverable, so a
        # strict run can suppress the known ones (tests/compat/ubsan-legacy.supp)
        target_compile_options(${name}_mod PRIVATE -fsanitize-recover=undefined,float-cast-overflow)
    endif()
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
