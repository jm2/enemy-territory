/* What q_platform.h would define on Classic Mac OS with Retro68, a platform the
 * vendored file does not know. Force-included for the Mac build so that file
 * stays as released. Paths through the engine's filesystem stay '/'. */
#ifndef ET_MAC_PLATFORM_H
#define ET_MAC_PLATFORM_H
#define OS_STRING "macos"
#define ARCH_STRING "ppc"
#define ID_INLINE inline
#define PATH_SEP '/'
#define DLL_EXT "_mac"
#define Q3_BIG_ENDIAN
#define CPUSTRING "macos-ppc"
#endif
