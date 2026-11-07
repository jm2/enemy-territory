# Classic Mac OS with Retro68, as Quake III's port builds it. See that port's
# CMakeLists.txt for why each flag is here: -fsigned-char matches retail,
# no section GC (MakePEF needs every XCOFF section), and Release adds no -O.
# build_mac.sh passes -DET_OPT=-O2 unless told otherwise, so a cached -O0 never sticks;
# -DET_OPT=-O0 is the bring-up build, easier to read in MacsBug.
# The flags beside it keep what the code relies on, at any -O, as Quake III's port does:
#   -fwrapv: signed overflow wraps (id's code assumes it; an LCG miscompiled without it there)
#   -fno-delete-null-pointer-checks: low memory is mapped on Mac OS 9, so a NULL
#       dereference carries on and a later NULL check still has to run
#   -ffp-contract=off: no fused multiply-add, so Pmove matches x86 peers and servers
#   -fno-strict-aliasing: the engine and mods have it already; this reaches the splines
#       C++, the console hooks and the installer, which pun floats too
set(ET_OPT "-O2" CACHE STRING "Optimization for the Mac build")
set(ET_SAFE_OPT "-fwrapv -fno-delete-null-pointer-checks -ffp-contract=off -fno-strict-aliasing")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ET_OPT} ${ET_SAFE_OPT} -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ET_OPT} ${ET_SAFE_OPT} -g -fsigned-char")
# every build type's flags, not only Release's: RelWithDebInfo or MinSizeRel would add an -O after
# ET_OPT and win, so -DET_OPT=-O0 would be lost
foreach(config RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
    set(CMAKE_C_FLAGS_${config} "")
    set(CMAKE_CXX_FLAGS_${config} "")
endforeach()
set(ET_MOD_DEFS __MACOS__ __POWERPC__)
list(APPEND ET_MOD_FLAGS "SHELL:-include ${ET_LEGACY}/static/mac_platform.h")
# The XCOFF TOC holds 64 KB. Three modules and the engine in one image need about
# 85 KB, so each module object takes its own table and one TOC slot.
list(APPEND ET_MOD_FLAGS -mminimal-toc)
