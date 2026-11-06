# Running on a real Mac

Install and test one subsystem at a time. Based on the Quake III port's guide, which
covers MacsBug, crashes and logs in more detail.

## Install

`./build_mac.sh package --bringup` writes everything to `release_mac/`:

| File | Use |
| --- | --- |
| `ET_Install.img.bin` | Application, game data, and the stage 1 `METParms.txt` and `bringup.cfg` |
| `ET_Bin.img.bin` | Application only, for redeploying after a rebuild |
| `package.txt` | Commit, whether the tree was modified, and the images' SHA-256 |
| `symbols/` | `ET.xcoff` and the manifest. Stays on the build machine for crash reports |

Leave out `--bringup` for an image with no `METParms.txt`, which starts the game normally,
without sound until `s_initsound` is set (stage 4).

Decode the MacBinary file with StuffIt Expander, mount the image with Disk Copy 6.5, as for
Quake III's images, and copy the `Enemy Territory` folder to a writable disk. That route is not
yet tried for ET. The test VM has no Disk Copy, and the image has only been mounted there as a
CD, which shows the same locked volume. The volume is locked, and the game writes `etkey`, its config and its logs
beside itself. The tree on disk is:

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
