# 32-bit big-endian PowerPC Linux, run under qemu-user. See scripts/ppc.sh.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(CMAKE_C_COMPILER powerpc-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc-linux-gnu-g++)
set(CMAKE_OBJCOPY powerpc-linux-gnu-objcopy)
# -msdata=none: small globals in .sdata and .sbss would escape the module brackets
set(CMAKE_C_FLAGS_INIT "-static -I/hostinc -msdata=none -G0")
set(CMAKE_CXX_FLAGS_INIT "-static -I/hostinc -msdata=none -G0")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
