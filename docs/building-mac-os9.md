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
./build_mac.sh                  # -O2, the default
./build_mac.sh --bringup        # -O0, easier to read in MacsBug, as Quake III's --bringup
```

`-fwrapv -fno-delete-null-pointer-checks -ffp-contract=off` go with any `-O`, as in Quake III's
port: id's code assumes wrapping overflow, low memory is mapped on Mac OS 9 so NULL checks must
stay, and no fused multiply-add keeps movement matching x86 servers. build_mac.sh sets
`ET_OPT` on every run, `-O0` with `--bringup` and `-O2` without, so the CMake cache never keeps
the last one. An `-DET_OPT=` among the options wins over both.

| Output, in `build-mac/` | Use |
| --- | --- |
| `ET.bin` | MacBinary, for the Mac |
| `ET.xcoff` | Symbols for crash reports. Keep the one from the same build |
| `ET.ad`, `%ET.ad` | AppleDouble pair |
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
- **Partition.** The SIZE resource in `src/mac9/mac_resources.r` is 264 MiB minimum and
  293 MiB preferred. Do not raise it casually: OpenGL's texture copies live in
  temporary memory outside the partition, and a Mac needs RAM left for them.
- **One patch to the mod.** `MAX_ANIMSCRIPT_MODELS` is 8, not 32, which saves 54 MB.
  `scripts/sync_legacy.sh` applies it.

## Packaging

```sh
./build_mac.sh package             # -O2, a package that starts the game normally
./build_mac.sh package --bringup   # -O0, with the stage 1 files of docs/target-run.md
```

This needs `genisoimage` or `mkisofs`, and the game data in `$ET_DATA_DIR`, which holds `etmain/`
and `legacy/` and defaults to `~/et-data/run/base`. The data is never committed. Each file must
match `mac-data/game-data.sha256`, so a demo pak or another mod release is refused before the
build starts. The paks are at `https://mirror.etlegacy.com/etmain/`, and the mod pk3 is in the
ET: Legacy v2.86.2 release.

The image is a genisoimage HFS/ISO hybrid, wrapped by `ndif_image.py` as a Disk Copy read/write
(NDIF) image typed `dimg`/`ddsk`, so the Disk Copy in Mac OS 9 mounts it. The script is Quake
III's (`833d8eb0`), copied verbatim; keep the two the same. Its docstring has the layout, which was
read back from images Disk Copy 6.3.3 made.

`release_mac/` is rebuilt from nothing each time, and the image and the installer are both read back:
the application must match the build, and the data must match the checksums. `docs/target-run.md` lists the
output. A plain build needs no game data.

Every build also writes `build-mac/ET.manifest.txt` with the PEF's SHA-256 and the toolchain,
and `cmake/mac_partition.py` fails it if the SIZE minimum cannot hold the linked image and the
engine's fixed demand with headroom. `retro68_commit` reads `unknown` when `tools/Retro68-src`
has no git checkout, as it does here, and `retro68_pinned` is then `no`.

`tests/package/run.sh` checks the refusals, the partition arithmetic and the installer format. There is no CI, so run it
by hand after changing a memory default.

`package.txt` records the commit (`commit=`), whether the tree was modified (`tree=`), `ET_OPT`
(`opt=`), whether it has the stage 1 files (`bringup=`), the PEF's SHA-256 and each file's.
`./build_mac.sh package --bringup -DET_OPT=-O2` is an optimized build with the stage 1 files; it
needs the `-DET_OPT=-O2` on every run, or the next `--bringup` is `-O0` again. The next package deletes `release_mac/`, including the symbols
of the image already on the Mac, so copy `symbols/` somewhere first if that image is still in use.
