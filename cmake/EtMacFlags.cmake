# Classic Mac OS with Retro68, as Quake III's port builds it. See that port's
# CMakeLists.txt for why each flag is here: -fsigned-char matches retail,
# no section GC (MakePEF needs every XCOFF section), and Release adds no -O.
set(ET_OPT "-O0" CACHE STRING "Optimization for the Mac build")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ET_OPT} -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ET_OPT} -g -fsigned-char")
set(CMAKE_C_FLAGS_RELEASE "")
set(CMAKE_CXX_FLAGS_RELEASE "")
set(ET_MOD_DEFS __MACOS__ __POWERPC__)
list(APPEND ET_MOD_FLAGS "SHELL:-include ${ET_LEGACY}/static/mac_platform.h")
# The XCOFF TOC holds 64 KB. Three modules and the engine in one image need about
# 85 KB, so each module object takes its own table and one TOC slot.
list(APPEND ET_MOD_FLAGS -mminimal-toc)
