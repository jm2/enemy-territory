# Running on a real Mac

Install and test one subsystem at a time. Based on the Quake III port's guide, which
covers MacsBug, crashes and logs in more detail.

## Install

Keep `build-mac/ET.xcoff` on the build machine, and take `ET.bin` to the Mac. On a
writable disk:

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

- Get Info needs at least 264 MB (293 MB preferred). Leave it there: OpenGL keeps its
  texture copies in RAM outside the partition, and a bigger partition starves it.
- The paks are from `https://mirror.etlegacy.com/etmain/`, the mod pk3 from the
  v2.86.2 release. `etmain:etkey` is created on the first launch.
- Thousands of colors for the windowed stages.

An `etconfig.cfg` from an earlier build keeps its `com_hunkMegs` and `com_zoneMegs`.
Delete it between builds if the memory defaults changed.

## Logs

As Quake III's, renamed: `etboot.log` and `etboot.prev.log` beside the application.
Copy them out before launching again.

```sh
python3 mac_symbolize.py etboot.log build-mac/ET.xcoff
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
