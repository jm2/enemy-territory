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

`CLIENT=./et_host` runs a build of this tree instead of the retail client, with
`et_host` copied into `run/base`. Logs land in `$ET_DATA/compat-out`. Needs bwrap, Xvfb, and i686 Mesa and X11 libraries.
