# Building for Mac OS 9

The build makes one PowerPC Classic application with Retro68: the engine and the
`legacy` modules in a single PEF.

## Toolchain

The same pinned Retro68 as the Quake III port (`retro68-versions.txt`, GCC 12.2.0).
`./setup_retro68.sh` builds it into `tools/`. A copy of a finished `tools/Retro68-build`
works too, with `tools/Retro68-src/Console` and
`tools/Retro68-src/InterfacesAndLibraries/SharedLibraries` beside it.

## Build

```sh
./build_mac.sh                  # -O0
./build_mac.sh -DET_OPT=-O2     # after -O2 is proven on hardware
```

| Output, in `build-mac/` | Use |
| --- | --- |
| `ET.bin` | MacBinary, for the Mac |
| `ET.xcoff` | Symbols for crash reports. Keep the one from the same build |
| `ET.dsk`, `ET.ad`, `%ET.ad` | Disk image and AppleDouble pair |
| `ET.static-modules.txt` | The module reset ranges and duplicate-symbol check |

## Things that differ from Quake III's build

- **Symbols.** Each module's globals are renamed by `legacy/static/rename_<module>.h`.
- **TOC.** The image needs about 85 KB of TOC and XCOFF allows 64 KB, so modules build
  with `-mminimal-toc`. Module code pays one extra load for its first global access
  in each function.
- **Platform header.** `legacy/static/mac_platform.h` stands in for the parts of
  `q_platform.h` that do not know Classic Mac OS, so that file stays as released.
- **Warnings.** Engine and platform code build with no warnings. The vendored modules
  build with `-w`.
- **Partition.** The SIZE resource in `src/mac9/mac_resources.r` is 300 MB minimum and
  340 MB preferred. Module bss is 146 MB of that, half of it two copies of
  `animScriptData_t`. Nothing checks it against the link yet.
