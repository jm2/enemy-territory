# 2.60b compatibility harness

Runs a retail or built ET client against a release ET: Legacy server, with the
sandbox limited to loopback and the test tree. Game data stays outside the repo.
See `docs/port-plan.md` for what it has shown.

`ET_DATA` (default `~/et-data`) must hold:

```text
run/base/        etlded.x86_64, et.x86, etmain/pak0-2.pk3 and the ETL etmain cfgs,
                 legacy/ from the release, plus cgame.mp.i386.so and ui.mp.i386.so
                 extracted from legacy_v2.86.2.pk3
run/clhome/.etwolf/legacy/drive.cfg   (the sample here)
run/svhome/
```

Then:

```sh
tests/compat/run.sh                                          # run A: expect Bad GUID
GUIDCHECK=0 tests/compat/run.sh                              # run B
CLGUID=0123456789ABCDEF0123456789ABCDEF tests/compat/run.sh  # run C
```

`tests/compat/master.sh` runs `globalservers` against two fake masters and a host that answers
unasked, with no server, and checks the list. Its logs land in `$ET_DATA/compat-master`.

`CLIENT=./et_host` runs a build of this tree instead of the retail client, with
`et_host` copied into `run/base`. Logs land in `$ET_DATA/compat-out`. Needs bwrap, Xvfb, and i686 Mesa and X11 libraries.

A real UDP download: put a pk3 only the server has in `run/svhome/legacy`, holding a file
the server loads so a pure server references it. `maps/oasis.script` from
`legacy_v2.86.2.pk3`, zipped alone as `zz_dltest.pk3`, does. Then

```sh
DL=1 GUIDCHECK=0 CLIENT=./et_host tests/compat/run.sh
```

The client log shows `Need paks: legacy/zz_dltest.pk3` and the client enters the game, and
`run/clhome/.etwolf/legacy/zz_dltest.pk3` matches the server's. Remove both copies afterwards.

## Sanitizer runs

The host build takes `-DET_SANITIZE=address,undefined,float-cast-overflow` (any `-fsanitize=`
list), and `-DET_SANITIZE_STRICT=ON` for `-fno-sanitize-recover=undefined,float-cast-overflow`.
The `legacy` modules stay recoverable in a strict build, so a strict run can suppress their known
reports (`ubsan-legacy.supp`). Fedora ships the i686 sanitizer runtimes only as packages that are
not installed here, so `-DET_SANITIZE_LIBDIR` links them statically from a directory holding
`libasan.a` and `libubsan.a`, taken from the `libasan-static` and `libubsan-static` i686 RPMs of the
host's own gcc version (`dnf download --arch i686`, then `rpm2cpio | cpio -id`):

```sh
cmake -S . -B build-san -DET_TARGET=host -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DET_ARCH_FLAGS=-m32 \
  -DET_SANITIZE=address,undefined,float-cast-overflow -DET_SANITIZE_LIBDIR=$HOME/et-data/sanitizer-libs/i686
cmake --build build-san --target et_host
tests/compat/sanitize.sh build-san/et_host ~/et-data/evidence/sanitizer-run/mine            # survey
STRICT=1 tests/compat/sanitize.sh build-san-strict/et_host ~/et-data/evidence/sanitizer-run/strict
```

`sanitize.sh` runs the menus, the six retail maps on a pure server, a listen server (`map`, a map
change, `map_restart`, `devmap` and the in-game menus), the UDP download above and `vid_restart`,
`snd_restart`, disconnect and reconnect, with the fake sound ring on. `run.sh` takes `MAP`, `DRIVE`
(the cfg to exec), `SND` and `SV=0` (no dedicated server) for it. The sanitizer logs land beside
each scenario's client log. ASan does not see inside the hunk and the zone, which are carved from
one allocation.
