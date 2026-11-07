# Running on a real Mac

Install and test one subsystem at a time. Based on the Quake III port's guide, which
covers MacsBug, crashes and logs in more detail.

## Install

`./build_mac.sh package --bringup` builds at `-O0` and writes everything to `release_mac/`:

| File | Use |
| --- | --- |
| `ET_Installer.bin` | One-file install: application, game data, and the stage 1 `METParms.txt` and `bringup.cfg` |
| `ET.bin` | The bare application, for redeploying after a rebuild |
| `ET_Install.img.bin` | The same install as a disk image, kept as a fallback. Mounting it needs Toast |
| `package.txt` | Commit, whether the tree was modified, `-O` level, whether it has the stage 1 files, and each file's SHA-256 |
| `symbols/` | `ET.xcoff` and the manifest. Stays on the build machine for crash reports |

Leave out `--bringup` for the `-O2` package with no `METParms.txt`, which starts the game
normally, without sound until `s_initsound` is set (stage 4).

`-O0`, as in Quake III's bring-up builds, keeps every variable in memory and plain stack frames,
so a crash or hang reads more easily in MacsBug. The `-O2` build has not run on hardware yet. Once
the stages pass at `-O0`, `./build_mac.sh package --bringup -DET_OPT=-O2` repeats them optimized;
a difference between the two points at the optimizer. `package.txt` records which one a package
is (`opt=`).

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
Copy them out before launching again. The log starts before `METParms.txt` is read, so a
launch that stops while reading it still leaves one.

```sh
python3 mac_symbolize.py etboot.log release_mac/symbols/ET.xcoff
```

A crash logs whether the fault was a read, write or execute, and for a memory fault its address.
Trust `fault address` over `DAR`, `DSISR` and `MSR`, which were wrong under emulation.

Commands typed at the console in a game need a leading `/`: plain text is chat.

Multitexture and compiled vertex arrays are on where the driver has them, and the log says
`...using` for each. If something renders wrongly or hangs, `+set r_ext_multitexture 0` and
`+set r_ext_compiled_vertex_array 0` turn them off one at a time.

Anisotropic filtering is on in a new config, at level 1: plain filtering, one extra texture
parameter per upload, not measured on the mini. A config saved before keeps its old value. The
menu's AF level, or `r_textureAnisotropy`, raises it; `+set r_ext_texture_filter_anisotropic 0`
turns it off. A line `anisotropy out of range` at startup and after `vid_restart` is stock ET: the
level still applies.

S3TC texture compression is used if the driver lists `GL_EXT_texture_compression_s3tc`. The
emulator's Radeon doesn't, and whether the mini's Radeon 9200 does under Mac OS 9 is unknown, so
look for `...using` or `...not found` for it in the log. ET never compresses mipmapped textures on
Mac, so world textures are unaffected: only 2D art larger than 128x128 is, meaning menus, the HUD,
level shots and the font pages. If it is in use, check that menu text and the HUD are sharp. If
they are blocky, use `+set r_ext_compressed_textures 0`.

## Stages

On the emulator (ClassicMac, 512 MB) stages 1 to 3 pass, with sound muted there, and stage 5
against a `legacy` server in the same VM. Its colors are wrong in a window with 32-bit textures,
which is the emulator's, and it says nothing about a real card or real speed.

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

The log lists the refresh rates the display offers for a mode, and the one used, before the
display changes. The game takes the fastest rate up to 85 Hz that DrawSprocket trusts, or else
the slowest trusted rate above that. If it trusts none, it takes the slowest rate of at least
60 Hz, or the fastest below 60 Hz if that is all there is. `+set r_displayRefresh 85` asks for
one.

**If DrawSprocket doesn't trust the mode, the game asks whether you can read a prompt.** Press Y
to keep it, and it is not asked again. N, Escape or 15 seconds without a Y put the display back
and fall back to 640x480x16. If the monitor loses sync, wait, and don't press Y blind. On the
Quake III port, a Mac mini G4 with a CRT went out of range at 1024x768 before this prompt
existed. The fallback isn't asked about and ignores `r_displayRefresh`; if it is out of range
too, restart the Mac and set a mode in `METParms.txt`. `+set r_displayConfirm 1` asks about
every mode, to test the prompt; clear `r_displayConfirmed` or start with `safe` to be asked
about a mode already confirmed.

### 3. Mouse and keyboard, menus

No `+exec`. Expect the mod's menus and the `SECURITY INFORMATION` label, which is
expected on this engine. Create a profile, then start `oasis` from the menu and play.
The log names each input device and its elements. `using ... axis element(s)` means no device
reports delta movement, so axes are read as mouse movement, and the view may turn on its own.

### 4. Sound

Add `+set s_initsound 1`, and try `snd_restart` several times.

### 5. Network

Leave out `net_noudp`, and `connect` to a `legacy` v2.86.2 server. A server on any
other release is refused with a message. `Dropped a packet with no source address` is harmless:
such packets are not used. Send it along with its numbers, as the cause is not known.
