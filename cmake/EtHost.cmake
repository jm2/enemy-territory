# Headless host build: real Unix platform and sockets, no-op GL, fake sound ring.
set(ET_HOST_FLAGS -g -O1 -fno-pie)
add_compile_options(${ET_HOST_FLAGS})
add_link_options(-no-pie)

# ELF brackets for vm_static.c: <name>_mod merged, then its .data and .bss given
# section names the linker publishes as __start_/__stop_ symbols.
function(et_bracket name)
    set(obj ${CMAKE_BINARY_DIR}/${name}_bracketed.o)
    add_custom_command(OUTPUT ${obj}
        COMMAND ${CMAKE_C_COMPILER} ${ET_ARCH_FLAGS} -r -nostdlib -no-pie -Wl,--whole-archive $<TARGET_FILE:${name}_mod> -Wl,--no-whole-archive -o ${obj}
        COMMAND ${CMAKE_OBJCOPY} --rename-section .data=moddata_${name} --rename-section .bss=modbss_${name} ${obj}
        DEPENDS ${name}_mod VERBATIM)
    add_custom_target(${name}_bracket DEPENDS ${obj})
    set(ET_BRACKETED_${name} ${obj} PARENT_SCOPE)
endfunction()
et_bracket(cgame)
et_bracket(ui)
et_bracket(qagame)

et_prefix(ET_UNIX unix linux_signals.c unix_main.c unix_net.c unix_shared.c)
set(ET_ENGINE_COMMON ${ET_SERVER} ${ET_QCOMMON} ${ET_SHARED} ${ET_BOTLIB} ${ET_UNIX} ${CMAKE_SOURCE_DIR}/tests/host/host_common.c)

add_library(et_splines STATIC ${ET_SPLINES})
target_compile_options(et_splines PRIVATE -w "$<$<COMPILE_LANGUAGE:C>:-std=gnu99;-fgnu89-inline;-fpermissive>")

function(et_engine target)
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C>:${ET_C_FLAGS};-fpermissive>" -w)
    target_compile_definitions(${target} PRIVATE BOTLIB ET_STATIC_MODULES C_ONLY)
    target_include_directories(${target} PRIVATE ${ET_SRC})
    target_link_libraries(${target} PRIVATE et_splines m dl)
endfunction()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ET_GL_TABLE ${CMAKE_BINARY_DIR}/host_gl_table.c)
add_custom_command(OUTPUT ${ET_GL_TABLE} COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_host_gl.py ${ET_GL_TABLE}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_host_gl.py ${ET_SRC}/renderer/qgl.h VERBATIM)

add_executable(et_host ${ET_ENGINE_COMMON} ${ET_GL_TABLE} ${ET_JPEG} ${ET_RENDERER} ${ET_CLIENT}
    ${ET_SRC}/qcommon/dl_main_stubs.c ${CMAKE_SOURCE_DIR}/tests/host/host_client.c
    ${ET_BRACKETED_cgame} ${ET_BRACKETED_ui} ${ET_BRACKETED_qagame})
et_engine(et_host)
add_dependencies(et_host cgame_bracket ui_bracket qagame_bracket)

add_executable(etded_host ${ET_ENGINE_COMMON}
    ${ET_SRC}/null/null_client.c ${ET_SRC}/null/null_input.c ${ET_SRC}/null/null_snddma.c
    ${ET_SRC}/qcommon/dl_main_stubs.c ${ET_BRACKETED_qagame})
et_engine(etded_host)
target_compile_definitions(etded_host PRIVATE DEDICATED)
add_dependencies(etded_host qagame_bracket)
