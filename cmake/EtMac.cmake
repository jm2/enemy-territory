# Classic Mac OS application: XCOFF, then MakePEF, then Rez, as Quake III's port.
add_definitions(-D__MACOS__ -D__POWERPC__ -DBOTLIB -DET_STATIC_MODULES)
link_directories("${CMAKE_SOURCE_DIR}/tools/Retro68-build/powerpc-apple-macos/lib"
    "${CMAKE_SOURCE_DIR}/tools/Retro68-src/InterfacesAndLibraries/SharedLibraries")

set(ET_MAC ${ET_SRC}/mac9)
set(ET_MAC_SOURCES ${ET_MAC}/mac_bootlog.c ${ET_MAC}/mac_console.c ${ET_MAC}/mac_consolehooks.cc ${ET_MAC}/mac_crash.c
    ${ET_MAC}/mac_event.c ${ET_MAC}/MacGamma.c ${ET_MAC}/mac_glimp.c ${ET_MAC}/mac_input.c ${ET_MAC}/mac_main.c
    ${ET_MAC}/mac_net.c ${ET_MAC}/mac_snddma.c ${ET_MAC}/mac_syscalls.c)
set_source_files_properties(${ET_MAC}/mac_consolehooks.cc
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/tools/Retro68-src/Console")

find_package(Git QUIET)
set(ET_BUILD_REV "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short=10 HEAD
        OUTPUT_VARIABLE et_git_rev OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE et_git_result ERROR_QUIET)
    if(et_git_result EQUAL 0)
        set(ET_BUILD_REV "${et_git_rev}")
    endif()
endif()
set_source_files_properties(${ET_MAC}/mac_main.c PROPERTIES COMPILE_DEFINITIONS "Q3_BUILD_REV=\"${ET_BUILD_REV}\"")

add_library(et_splines STATIC ${ET_SPLINES})
target_compile_options(et_splines PRIVATE -w "$<$<COMPILE_LANGUAGE:C>:-std=gnu99;-fgnu89-inline>")

add_executable(ET ${ET_SERVER} ${ET_QCOMMON} ${ET_SHARED} ${ET_BOTLIB} ${ET_JPEG} ${ET_RENDERER} ${ET_CLIENT}
    ${ET_SRC}/qcommon/dl_main_stubs.c ${ET_MAC_SOURCES} ${ET_SRC}/mac9/mac_et.c)
# A format string a peer can influence must never reach printf: these make any
# such call, and any argument that does not match its format, a build failure.
target_compile_options(ET PRIVATE "$<$<COMPILE_LANGUAGE:C>:${ET_C_FLAGS};-Wformat=2;-Wno-format-nonliteral;-Werror=format;-Werror=format-security;-Wno-error=format-overflow;-Wno-error=format-truncation>")
target_include_directories(ET PRIVATE ${ET_SRC} ${ET_SRC}/game ${ET_SRC}/cgame ${ET_SRC}/client ${ET_SRC}/server
    ${ET_SRC}/renderer ${ET_SRC}/qcommon ${ET_SRC}/botlib ${ET_MAC})
target_link_libraries(ET PUBLIC
    "-Wl,--whole-archive" cgame_mod ui_mod qagame_mod "-Wl,--no-whole-archive"
    et_splines -lm -lInterfaceLib -lOpenTransportLib -lOpenTransportAppPPC -lOpenTptUtilsAppPPC -lOpenTptInternetLib
    -lInputSprocketLib -lDrawSprocketLib -lOpenGLLibraryStub -lSoundLib -lRetroConsole -lstdc++)

# ---- static module brackets: see cmake/static_modules.py and vm_static.c
get_filename_component(RETRO68_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(RETRO68_LD powerpc-apple-macos-ld HINTS "${RETRO68_BIN_DIR}")
find_program(RETRO68_NM powerpc-apple-macos-nm HINTS "${RETRO68_BIN_DIR}")
find_program(RETRO68_MAKEPEF MakePEF HINTS "${RETRO68_BIN_DIR}")
find_program(RETRO68_REZ Rez HINTS "${RETRO68_BIN_DIR}")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(ET_STATIC_TOOL "${CMAKE_SOURCE_DIR}/cmake/static_modules.py")
set(ET_DEFAULT_LDSCRIPT "${CMAKE_BINARY_DIR}/retro68-default-ldscript.txt")
execute_process(COMMAND "${RETRO68_LD}" --verbose OUTPUT_FILE "${ET_DEFAULT_LDSCRIPT}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "cannot read Retro68's default linker script")
endif()
set(ET_LDSCRIPT "${CMAKE_BINARY_DIR}/ET.static-modules.x")
set(et_modules "")
set(et_check_modules "")
foreach(name qagame cgame ui)
    list(APPEND et_modules --module "${name}=lib${name}_mod.a")
    list(APPEND et_check_modules --module "${name}=$<TARGET_FILE:${name}_mod>")
endforeach()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${ET_STATIC_TOOL}" ldscript --default "${ET_DEFAULT_LDSCRIPT}"
        --ld "${RETRO68_LD}" --output "${ET_LDSCRIPT}" ${et_modules}
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "cannot write the static module linker script:\n${error}")
endif()
target_link_libraries(ET PRIVATE "-Wl,-T,${ET_LDSCRIPT}" "-Wl,-Map,${CMAKE_BINARY_DIR}/ET.map")
set_property(TARGET ET APPEND PROPERTY LINK_DEPENDS "${ET_LDSCRIPT}")
set_target_properties(ET PROPERTIES OUTPUT_NAME "ET.xcoff")

add_custom_command(OUTPUT ET.static-modules.txt
    COMMAND "${Python3_EXECUTABLE}" "${ET_STATIC_TOOL}" check --map ET.map --nm "${RETRO68_NM}" ${et_check_modules}
        --override ._open_r=mac_syscalls.c.obj --override ._rename_r=mac_syscalls.c.obj
        --override ._unlink_r=mac_syscalls.c.obj --override ._consolewrite=mac_consolehooks.cc.obj
        --override ._consoleread=mac_consolehooks.cc.obj --output ET.static-modules.txt
    DEPENDS ET "${ET_STATIC_TOOL}" WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Checking the static module brackets" VERBATIM)

# ---- PEF and the application, as Quake III's port does it
add_custom_command(OUTPUT ET.pef
    COMMAND "${RETRO68_MAKEPEF}" "$<TARGET_FILE:ET>" -o ET.pef
    DEPENDS ET ET.static-modules.txt COMMENT "Converting ET to PEF" VERBATIM)
set(ET_REZ_INCLUDES "${CMAKE_SOURCE_DIR}/tools/Retro68-build/universal/RIncludes")
add_custom_command(OUTPUT ET.bin ET.dsk ET.ad %ET.ad
    COMMAND "${RETRO68_REZ}" "${ET_MAC}/mac_resources.r" -I "${ET_REZ_INCLUDES}" -I "${ET_MAC}"
        -t APPL -c WfET --data ET.pef -o ET.bin --cc ET.dsk --cc %ET.ad
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/mac_app.py" finalize --creator WfET --pef ET.pef ET.bin ET.dsk %ET.ad
    DEPENDS ET.pef ${ET_MAC}/mac_resources.r ${ET_MAC}/et_icons.r "${CMAKE_SOURCE_DIR}/mac_app.py"
    COMMENT "Building the Classic application" VERBATIM)
add_custom_target(ET_APPL ALL DEPENDS ET.bin ET.dsk ET.ad %ET.ad)
