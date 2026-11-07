# Enemy Territory on Mac OS 9: port plan

Last updated: 2026-11-07. Target mod: `legacy` v2.86.2 (ET: Legacy tag `v2.86.2`,
`fdb40f87`). Engine base: id's 2.60 GPL release in `src/`. Platform base: the
Quake III Arena Mac OS 9 port (`~/Quake-III-Arena`), copied at `72dfd69f` and its Mac layer
merged up to `496b52ee`.

"Verified" below means run on this host. The Mac OS 9 results so far are from an emulator
(`docs/target-run.md`); nothing has run on real hardware.

## Does `legacy` v2.86.2 run on a 2.60b engine?

**Yes, on the retail 2.60b binary, with one server setting in the way.** The test
is `tests/compat/`: retail `et.x86` (May 8 2006) with the release
`cgame.mp.i386.so` and `ui.mp.i386.so`, against the release v2.86.2 `etlded`, in a
loopback-only sandbox under Xvfb with llvmpipe.

| Run | Server settings | Client | Result |
| --- | --- | --- | --- |
| A | defaults (`g_guidCheck 1`, `sv_pure 1`) | `cl_guid` "unknown" | **Rejected:** `Bad GUID: Invalid etkey. Use the ET: Legacy client or enable PunkBuster` |
| B | `g_guidCheck 0`, `sv_pure 1` | `cl_guid` "unknown" | Joined, spawned on Allies on `oasis`, HUD and limbo correct, clean quit |
| C | defaults | well-formed 32-hex `cl_guid` | Joined, `oasis`, HUD correct, clean quit |

Not covered: one client for about a minute, no sound, no bots, no other maps, no
pk3 auto-download (the pk3 was preinstalled), `fs_game legacy` preset rather than
followed from the server, x86 only, and the retail binary rather than a build of
this GPL tree.

Static checks agree with the runs:

- `entityStateFields` and `playerStateFields` in `msg.c` are identical in both
  trees, as are `playerState_t` and `entityState_t` layouts and all compared limits
  including `MAX_RELIABLE_COMMANDS`, which the GPL source already has at 256.
- `cgame` and `ui` imports only gain entries at the end, and the mod reaches them
  through `trap_GetValue`, which it finds through a cvar probe and skips when the
  engine has none.
- `glconfig_t` is cut off at `smpActive` for mods, and `fontInfo_t` keeps the
  vanilla path when `etLegacyClient` is 0. The mod's `etLegacyClient` comes from the
  `etVersion` cvar, which 2.60 does not have.
- 2.60 cannot load `.svg`, but cgame does not register the `*_svg` shaders by
  name. The only asset failures in run C are the alternate images of crosshairs
  `s` and `t`, which is cosmetic.

## Engine additions needed

All are client-side and none changes the wire format.

| # | Addition | Why | Status |
| --- | --- | --- | --- |
| E1 | **`cl_guid` from an `etkey` file**: port `CL_GenerateETKey`, `CL_UpdateGUID`, `Com_MD5FileETCompat` from ETL `cl_main.c` and `md5.c` | Required. `src/` has no `cl_guid` code (PunkBuster set it in retail), and `g_guidCheck` defaults to 1 in `legacy.cfg`. Without this, default servers refuse the client. | **Done, verified.** Same scheme and file format as ETL's own client |
| E2 | **Pure checksums for static modules** | Required on pure servers. `SV_VerifyPaks_f` wants the checksum of the pk3 holding `cgame` and `ui`. `FS_ReferencedPakPureChecksums` only gets them from `FS_CGAME_REF`/`FS_UI_REF`, set when the DLL is opened from a pk3. A static client opens nothing, so `Sys_LoadDll` opens and closes `cgame.mp.i386.so` and `ui.mp.i386.so` by name. The release pk3 has every platform's DLLs; the server checks its own, in the same pk3. | **Done, verified:** without it the server logs `nChkSum1 0 == ...` and rejects the client as unpure |
| E3 | **Refuse mismatched mod versions** | A static cgame only matches one release. `CG_Init` compares `GAME_VERSION` only, so a `legacy_v2.86.1.pk3` server would load and could desync. The client checks `fs_game` and `sv_referencedPakNames` at connect. | **Done, verified** against a server with the pk3 renamed. Known limits: a server holding two legacy pk3s passes, and one with the pk3 in `etmain/` is refused. Neither is a standard install |
| E4 | ~~Raise `MAX_RELIABLE_COMMANDS` to 256~~ | Not needed. The GPL source is already 256; the first audit read the wrong header. | Closed |
| E5 | **Follow `cm_optimizePatchPlanes`** | ETL servers default to 0 and publish it in systeminfo; 2.60 always behaves as 1. Prediction could differ on curved surfaces. | Inferred, not seen in the runs. Measure before porting |

Found in stage 3, not yet fixed: the mod's menus focus an edit field by a name they build from two
strings, such as `"efleft" "Profile Name:"`, and this engine logs `Script_SetEditFocus: Can't find
efleftProfile Name:`. The x86 host build does the same, so it is not the Mac. The cause is not found: the menus build
the name with `"efleft" ## TEXT`, and this engine's string merging in `PC_MergeTokens` and
`PC_ReadToken` does the same as ET: Legacy's. Clicking the field still edits it, so a profile can be created, but keys typed before that go
to bindings (`t` is chat).

Expected and left alone: `OLD_CLIENT` shows a "SECURITY INFORMATION: You are
running old software" label and an `UPGRADE NOW` button, since `etLegacyClient` is
0. Removing them means changing the mod's menus or setting `etVersion`, which turns
on code paths that assume an ETL engine. Decide after the security pass.

Found while building, and also done:

- `GAME_INIT` passes `qtrue` and the release number, the two extra arguments ETL's
  engine passes. Without them the legacy qagame stops with `does not support server
  version`. They are read only by that check, and its extension traps are optional.
- `fs_game` defaults to `legacy`, as on ETL, because the linked-in ui needs its menus.

## Phase 1 results

| Run | Result |
| --- | --- |
| i386 host (`-m32`), local `oasis` with the static modules, `team`, `vid_restart`, disconnect, quit | Passes, exit 0 |
| i386 static client to the release x86-64 v2.86.2 `etlded`, default `g_guidCheck 1`, `sv_pure 1`, no `.so` on disk | Joins Allies, exit 0 |
| PPC32 big-endian under qemu-user, local `oasis` as above | Passes, exit 0 |
| PPC32 big-endian client to the release x86-64 `etlded`, as above | Joins Allies, exit 0 |
| PPC32 client to our own PPC32 `etded_host` | Joins Allies, exit 0 |
| PPC32 client to a server with `legacy_v2.86.1.pk3` | Refused with a message, exit 0 |

**Deviation from the phase 1 exit:** the host build is 32-bit x86 on the x86-64
machine, not a 64-bit binary. The engine passes pointers through `int` in the VM
syscall path, and the target is 32-bit, so 64-bit cleanup buys nothing the Mac needs.
All host runs use a no-op GL, so rendering is only proven by the retail runs above.

**Memory:** static data is about 41 MB in cgame, 39 MB in qagame and 12.7 MB in ui
(its pool doubles off x86, so 8.5 MB is the x86 figure),
mostly `cgs`, `level`, `cg_polyBuffers` and the qagame pool. All of it comes out of
the Mac's fixed partition, on top of the hunk.

## Facts that shape the build

- The release pk3 carries no Mac DLL, so static linking is the only route. It is
  also why the client still needs `legacy_v2.86.2.pk3` for assets.
- No curl on the Mac. `DL_*` becomes a stub, so `cl_wwwDownload` is off and
  downloads use the slow UDP path. Installing the pk3 by hand is the normal route.
- The port joins only `fs_game legacy` v2.86.2 servers: not `etmain`, other mods,
  other `legacy` releases, or PunkBuster and ETPro servers.
- Mod features to leave off: Lua, sqlite (`FEATURE_DBMS`), Omni-Bot, IRC, GeoIP.
  Each is checked in phase 1 for how much of `qagame` depends on it.
- `q_shared.h` in the mod differs from `src/game/q_shared.h`, so the mod builds
  against its own headers with `MODLIB`, never the engine's. ETL sources are
  vendored by tag so they can be rebuilt per release.
- Module arguments are `intptr_t`, which is `int` on 32-bit PPC.
- Each module's globals are renamed by `legacy/static/rename_<module>.h`, which
  `scripts/gen_static_renames.py` generates from `nm`, so the modules and engine can
  share one binary. Macros work the same for ELF and XCOFF.
- Module data and bss must be fully bracketed for the reset. On Linux PPC that takes
  `-msdata=none -G0`, or small globals land in `.sdata` and survive a reload.

## Inventory of `src/`

| Area | Files | Target build |
| --- | --- | --- |
| `qcommon`, `server`, `client`, `renderer` | engine | Keep; MDM/MDS/MDX renderer paths were on PPC under Aspyr |
| `botlib`, `botai` | server-side bots | `botlib` as `SConscript.core` builds it; legacy `qagame` replaces `botai` |
| `game`, `cgame`, `ui` | id's mods | Host reference only, replaced by `legacy` |
| `splines` | C++ camera splines | Keep |
| `jpeg-6`, `ft2` | bundled libs | `jpeg-6` keep; `ft2` only if `BUILD_FREETYPE`, the fonts ship as `.dat` |
| `curl-7.12.2`, `qcommon/dl_main_curl.c` | HTTP download | Dropped on the Mac (`DL_*` stub) |
| `bspc`, `extractfuncs` | tools | Not built |
| `null` | headless stubs | Useful for the host test link |
| `unix`, `win32` | platform | Host only (`unix`) |
| `mac` | Aspyr Carbon | Reference, not linked |
| `SCons*`, `*.vcproj`, `*.mcp` | old builds | Replaced by CMake for the target |

## Q3A `code/mac` to ET

| Q3A file | ET counterpart | Work |
| --- | --- | --- |
| `mac_main.c` | `mac/mac_main.cpp` (reference) | Adapt; static dispatch for three modules with `dllEntry`/`vmMain`, Parms file |
| `mac_net.c` | `mac/mac_net.cpp` | Copy; `Sys_SendPacket`, `Sys_GetPacket`, `Sys_StringToAdr`, `NET_Sleep` match. ET needs `Sys_ShowIP` |
| `mac_glimp.c`, `MacGamma.*` | `mac/mac_glimp.cpp`, `CDrawSprocket`, `AGLUtils` | Copy; ET asks for stencil and more extensions, check against GL 1.2.x |
| `mac_input.c`, `mac_event.c` | `mac/mac_input.cpp`, `mac_event.cpp` | Copy; add `Sys_IsNumLockDown`, ET key range |
| `mac_snddma.c` | `mac/mac_snddma.c` | Copy; off by default until stage 5 |
| `mac_console.c`, `mac_consolehooks.cc` | `mac/mac_console.c` | Copy |
| `mac_syscalls.c` | `mac/mac_files.cpp` | Copy unchanged, as far as possible |
| `mac_bootlog.*`, `mac_crash.c` | none | Copy unchanged |
| `mac_local.h`, `macprefix.h`, `mac_resources.r`, icons | `MacPrefix*.h`, `Wolfenstein ET.rsrc`, `Icon.icns` | Adapt; new creator code, ET icons, partition size |
| `cmake/`, `setup_retro68.*`, `check_retro68.*`, `retro68-versions.txt` | none | Copy, rename targets. Same pinned toolchain |
| `mac_app.py`, `macbinary_encode.py`, `create_appledouble.py`, `mac_symbolize.py` | none | Copy |
| `cmake/static_modules.py`, `vm_static.c` | none | Copy. Needed in ET too: a DLL reload starts fresh, a static module does not |

Platform functions the ET engine uses that Q3A `code/mac` does not provide, to
stub or write: `Sys_Chmod`, `Sys_Cwd`, `Sys_GetCurrentUser`, `Sys_GetDLLName`,
`Sys_GetProcessorId`, `Sys_GetCPUSpeed`, `Sys_GetHighQualityCPU`,
`Sys_ProcessorCount`, `Sys_IsNumLockDown`, `Sys_OpenURL`, `Sys_StartProcess`,
`Sys_In_Restart_f`, `Sys_RandomBytes`, `Sys_SetErrorText`, `Sys_DisplaySystemConsole`,
`Sys_ConcatenateFileLists`, `Sys_CountFileList`, `Sys_EndProfiling`, the
critical-section trio, `Sys_ShowIP` and `DL_*`. `Sys_GetBotLibAPI`,
`Sys_GetCGameAPI` and `Sys_Unload*` turn up in old code and may be unused.

## Questions for Q3A-OS9 (batch at phase 2)

- Fixes for "Oversize packet from 0.0.0.0:0" and the 1024x768 windowed fallback.
- `-O2`: the default since the owner asked for it on 2026-11-07, without waiting for Q3A's hardware
  runs. Emulator stages 1 and 5 pass at `-O2`; nothing has run on hardware.

## Rebuilding for a new ET: Legacy release

```sh
scripts/sync_legacy.sh ~/etlegacy <tag>
python3 scripts/gen_static_renames.py
```

Then update `legacy/static/version_generated.h` and the `ET_LEGACY_*` defines in
`src/qcommon/qcommon.h`, and rerun `tests/compat` and `tests/host/ppc_connect.sh`.
