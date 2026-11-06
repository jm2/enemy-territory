# Running on a real Mac

Install and test one subsystem at a time. Based on the Quake III port's guide, which
covers MacsBug, crashes and logs in more detail.

## Install

`./build_mac.sh package --bringup` writes everything to `release_mac/`:

| File | Use |
| --- | --- |
| `ET_Installer.bin` | One-file install: application, game data, and the stage 1 `METParms.txt` and `bringup.cfg` |
| `ET.bin` | The bare application, for redeploying after a rebuild |
| `ET_Install.img.bin` | The same install as a disk image, kept as a fallback. Mounting it needs Toast |
| `package.txt` | Commit, whether the tree was modified, and each file's SHA-256 |
| `symbols/` | `ET.xcoff` and the manifest. Stays on the build machine for crash reports |

Leave out `--bringup` for a package with no `METParms.txt`, which starts the game normally,
without sound until `s_initsound` is set (stage 4).

**First install.** Transfer `ET_Installer.bin` in binary mode and decode it with StuffIt Expander.
Put `ET Installer` on a writable disk and double-click it. It checks a CRC-32 of every file, and
only when all have passed does it name the result `Enemy Territory`, beside itself. A failed run
removes what it wrote, and says so if it cannot. A force quit, crash or power cut leaves a folder
called `ET being unpacked`: delete it and run the installer again. If you switch away while it
runs, its result waits behind the Finder until you switch back. It refuses to run beside an existing `Enemy Territory`
folder, so move that first; it never changes one. It needs about 270 MB of free disk beyond the
installer itself, and you can delete the installer afterwards.

**After a rebuild.** Transfer `ET.bin`, decode it with StuffIt Expander, and replace `ET` in the
folder. It is one file and keeps your `etkey`, config and logs.

**Disk image.** Mounting `ET_Install.img.bin` needs Toast. Copy the folder off it to a writable
disk: the volume is locked, and the game writes `etkey`, its config and its logs beside itself.

The tree on disk is:

```text
Enemy Territory:
    ET
    METParms.txt
    etmain:
        pak0.pk3  pak1.pk3  pak2.pk3
        bringup.cfg
    legacy:
        legacy_v2.86.2.pk3
```

- Leave Get Info at 264 MB (293 MB preferred). OpenGL keeps its texture copies in RAM outside
  the partition, and a bigger partition starves it. The 512 MB emulator needed exactly that.
  The Mac mini G4 has 1 GB, so it has more room for OpenGL than anything tested so far.
- `etmain:etkey` is created on the first launch.
- Thousands of colors for the windowed stages.

An `etconfig.cfg` from an earlier build keeps its `com_hunkMegs` and `com_zoneMegs`.
Delete it between builds if the memory defaults changed.

## Logs

As Quake III's, renamed: `etboot.log` and `etboot.prev.log` beside the application.
Copy them out before launching again.

```sh
python3 mac_symbolize.py etboot.log release_mac/symbols/ET.xcoff
```

## Stages

### 1. Window, no network, no sound

`METParms.txt`:

```text
+set r_fullscreen 0
+set net_noudp 1
+set com_introplayed 1
+exec bringup.cfg
```

`etmain:bringup.cfg`:

```text
set g_doWarmup 0
map oasis
wait 300
echo ==== stage: team
team b 1 3 12
wait 300
echo ==== stage: vid_restart
vid_restart
wait 300
echo ==== stage: quit
quit
```

Pass: `has joined the Allied team`, a second `CL_InitCGame` from the `vid_restart`,
`Sys_ShutdownNetworking();` at the end of `etboot.log`, and a return to the Finder. The
`echo` lines in `bringup.cfg` do not reach the log.

### 2. Fullscreen

As stage 1, without `r_fullscreen`. It should return to the desktop with the right
colors, after a quit and after a fatal error.

### 3. Mouse and keyboard, menus

No `+exec`. Expect the mod's menus and the `SECURITY INFORMATION` label, which is
expected on this engine. Create a profile, then start `oasis` from the menu and play.

### 4. Sound

Add `+set s_initsound 1`, and try `snd_restart` several times.

### 5. Network

Leave out `net_noudp`, and `connect` to a `legacy` v2.86.2 server. A server on any
other release is refused with a message.
