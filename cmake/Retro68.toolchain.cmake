# Retro68 Toolchain for PowerPC

set(CMAKE_SYSTEM_NAME Retro68)
set(CMAKE_SYSTEM_VERSION 1)
# CMake looks for Platform/<CMAKE_SYSTEM_NAME>.cmake on the module path and
# warns "System is unknown to cmake" without it (issue #232). As Retro68's own
# toolchain files do, ship one; the setup is all in this file.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

set(RETRO68_ROOT "${CMAKE_SOURCE_DIR}/tools/Retro68-build")
list(APPEND CMAKE_PREFIX_PATH "${RETRO68_ROOT}")

# Specify the cross compilers - find them in PATH or local build
find_program(CMAKE_C_COMPILER NAMES powerpc-apple-macos-gcc PATHS "${RETRO68_ROOT}/bin" NO_DEFAULT_PATH)
if(NOT CMAKE_C_COMPILER)
    find_program(CMAKE_C_COMPILER NAMES powerpc-apple-macos-gcc)
endif()

find_program(CMAKE_CXX_COMPILER NAMES powerpc-apple-macos-g++ PATHS "${RETRO68_ROOT}/bin" NO_DEFAULT_PATH)
if(NOT CMAKE_CXX_COMPILER)
    find_program(CMAKE_CXX_COMPILER NAMES powerpc-apple-macos-g++)
endif()

set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})

# Target environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

add_definitions(-D__MACOS__ -D__POWERPC__)
