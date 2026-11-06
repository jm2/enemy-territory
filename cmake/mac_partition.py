#!/usr/bin/env python3
"""Check the Classic application partition against the engine's fixed demand.

Mac OS 9 gives the application one fixed partition: the Finder grants the
SIZE resource's preferred size when it can and refuses to launch below its
minimum. Everything the engine allocates at startup has to fit in it at the
minimum, with room left for AGL/OpenGL, the Sound Manager, InputSprocket,
stdio and heap fragmentation. Retro68's malloc takes from the application
heap, and with virtual memory off CFM loads the code there too.

The fixed demand, with the Mac defaults read from the sources:

  hunk        com_hunkMegs (at least MIN_COMHUNKMEGS), one malloc of +31 bytes
  zone        com_zoneMegs (the Mac build has no 16 MiB floor)
  small zone  s_smallZoneTotal
  sound       com_soundMegs * 512 buffers of sizeof(sndBuffer), plus the
              4-chunk scratch buffer (SND_setup); sound may be turned on
  stack       the 'cfrg' application stack size
  image       the PEF's .text, .data and .bss, plus the static module
              snapshots (vm_static.c copies each module's .data)

  host   Fail unless the SIZE minimum covers the fixed demand from the
         sources, IMAGE_ALLOWANCE_KB for the image and HEADROOM_KB.
         tests/package/run.sh runs it, so that a larger default or a smaller
         SIZE fails without a build.
  map    The same, with the image measured from a linker map (--map) and
         required to stay within IMAGE_ALLOWANCE_KB, so a larger image fails
         the Retro68 build until the allowance and the SIZE are raised.

Both modes also check MAC_HUNK_RESERVE_KB, what Com_InitHunkMemory leaves
beside the hunk when it clamps com_hunkMegs to MaxBlock(): it must hold the
sound pool and RESERVE_ITEMS_KB, and at the SIZE minimum the default hunk must
still fit beside it, so the clamp never shrinks the default.
"""

import argparse
import re
import sys

IMAGE_ALLOWANCE_KB = 119 * 1024
# Left for AGL/OpenGL, the Sound Manager, InputSprocket and fragmentation.
HEADROOM_KB = 8 * 1024
# What MAC_HUNK_RESERVE_KB holds beside the sound pool, from its comment: the renderer's 24 MiB
# block, 6.3 MiB of listen server clients, and 21 MiB of image buffers for the mod's largest texture.
RESERVE_ITEMS_KB = 24 * 1024 + 6452 + 21 * 1024
# What the Mac build defines (and does not) when it compiles the engine.
MAC_DEFINES = {"__MACOS__"}


def fail(message):
    sys.stdout.flush()
    print("mac_partition: " + message, file=sys.stderr)
    sys.exit(1)


def read(path):
    with open(path, encoding="latin-1") as f:
        return f.read()


def evaluate(condition, defined):
    """Evaluate a #if expression made of defined(), !, &&, || and integers."""
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
                  lambda m: " 1 " if (m.group(1) or m.group(2)) in defined else " 0 ",
                  condition)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    if not re.fullmatch(r"[\s\d()]*(?:(?:and|or|not)[\s\d()]*)*", expr):
        return None
    return bool(eval(expr, {"__builtins__": {}}))


def mac_defines(path):
    """Return the #define NAME VALUE lines that the Mac build compiles in path.

    A conditional this cannot read hides everything up to its #endif."""
    stack = []          # (enclosing lines are live, a branch was taken, the condition was unreadable)
    live = True
    values = {}
    for line_number, line in enumerate(read(path).splitlines(), 1):
        m = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif|define)\b\s*(.*)", line)
        if not m:
            continue
        directive, rest = m.group(1), re.sub(r"//.*|/\*.*?\*/", "", m.group(2)).strip()
        if directive in ("ifdef", "ifndef", "if"):
            if directive == "ifdef":
                taken = rest.split()[0] in MAC_DEFINES
            elif directive == "ifndef":
                taken = rest.split()[0] not in MAC_DEFINES
            else:
                taken = evaluate(rest, MAC_DEFINES)
            stack.append((live, bool(taken), taken is None))
            live = live and taken is True
        elif directive in ("elif", "else"):
            if not stack:
                fail("%s:%d: #%s without #if" % (path, line_number, directive))
            parent, done, unknown = stack.pop()
            taken = True if directive == "else" else evaluate(rest, MAC_DEFINES)
            unknown = unknown or taken is None
            live = parent and not done and taken is True and not unknown
            stack.append((parent, done or bool(taken), unknown))
        elif directive == "endif":
            if not stack:
                fail("%s:%d: #endif without #if" % (path, line_number))
            live = stack.pop()[0]
        elif directive == "define" and live:
            parts = rest.split(None, 1)
            if parts:
                values[parts[0]] = parts[1] if len(parts) > 1 else ""
    return values


def number(values, name, path):
    if name not in values:
        fail("%s: the Mac build defines no %s" % (path, name))
    m = re.fullmatch(r'"?(\d+)"?', values[name].strip())
    if not m:
        fail("%s: %s is not a number: %s" % (path, name, values[name]))
    return int(m.group(1))


def search(pattern, path, text=None):
    m = re.search(pattern, read(path) if text is None else text, re.S)
    if not m:
        fail("%s: cannot find %r" % (path, pattern))
    return m


def arithmetic(expr, path):
    if not re.fullmatch(r"[\d\s*+()]+", expr):
        fail("%s: cannot evaluate %r" % (path, expr))
    return eval(expr, {"__builtins__": {}})


def fixed_demand(root):
    """Return [(item, bytes)] for everything but the image."""
    common = root + "/src/qcommon/common.c"
    snd_mem = root + "/src/client/snd_mem.c"
    snd_local = root + "/src/client/snd_local.h"
    resources = root + "/src/mac9/mac_resources.r"

    defs = mac_defines(common)
    hunk = max(number(defs, "DEF_COMHUNKMEGS", common), number(defs, "MIN_COMHUNKMEGS", common))
    zone = number(defs, "DEF_COMZONEMEGS", common)
    small = arithmetic(search(r"s_smallZoneTotal = ([\d\s*]+);", common).group(1), common)
    hunk_slack = int(search(r"s_hunkData = malloc\( s_hunkTotal \+ (\d+) \);", common).group(1))

    sound_megs = number(mac_defines(snd_mem), "DEF_COMSOUNDMEGS", snd_mem)
    per_meg = int(search(r"scs = cv->integer \* (\d+);", snd_mem).group(1))
    chunk = number(mac_defines(snd_local), "SND_CHUNK_SIZE", snd_local)
    search(r"typedef\s+struct sndBuffer_s \{\s*short\s+sndChunk\[SND_CHUNK_SIZE\];\s*"
           r"struct sndBuffer_s\s+\*next;\s*int\s+size;\s*adpcm_state_t\s+adpcm;\s*\} sndBuffer;",
           snd_local)
    # PPC32: the samples, a 4-byte pointer, an int and adpcm_state_t (short, char) padded to 4
    snd_buffer = chunk * 2 + 4 + 4 + 4
    scratch = chunk * 2 * 4

    stack = arithmetic(search(r"resource 'cfrg' \(0\).*?kNoVersionNum, kNoVersionNum,.*?\*/\s*([\d\s*]+),",
                              resources).group(1), resources)

    return [
        ("hunk (com_hunkMegs %d)" % hunk, hunk * 1024 * 1024 + hunk_slack),
        ("zone (com_zoneMegs %d)" % zone, zone * 1024 * 1024),
        ("small zone", small),
        ("sound pool (com_soundMegs %d, %d-byte buffers)" % (sound_megs, snd_buffer),
         sound_megs * per_meg * snd_buffer + scratch),
        ("cfrg stack", stack),
    ]


def hunk_reserve(root):
    common = root + "/src/qcommon/common.c"
    defs = mac_defines(common)
    if "MAC_HUNK_RESERVE_KB" not in defs:
        fail("%s: the Mac build defines no MAC_HUNK_RESERVE_KB" % common)
    return arithmetic(defs["MAC_HUNK_RESERVE_KB"], common) * 1024


def size_minimum(root):
    resources = root + "/src/mac9/mac_resources.r"
    body = search(r"resource 'SIZE' \(-1\) \{(.*?)\};", resources).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    sizes = [s.strip() for s in body.split(",")][-2:]
    preferred, minimum = (arithmetic(s, resources) for s in sizes)
    if preferred < minimum:
        fail("%s: the SIZE preferred size is below the minimum" % resources)
    return minimum, preferred


def map_image(path):
    """Return the .text, .data and .bss sizes and the static module snapshot from a linker map."""
    text = read(path)
    image = 0
    for section in (".text", ".data", ".bss"):
        m = re.search(r"^\%s\s+0x[0-9a-f]+\s+0x([0-9a-f]+)" % section, text, re.M)
        if not m:
            fail("%s: no %s output section" % (path, section))
        image += int(m.group(1), 16)
    for module in ("qagame", "cgame", "ui"):
        start = re.search(r"^\s+0x([0-9a-f]+)\s+etstatic_%s_data_start = \." % module, text, re.M)
        end = re.search(r"^\s+0x([0-9a-f]+)\s+etstatic_%s_data_end = \." % module, text, re.M)
        if not start or not end:
            fail("%s: no etstatic_%s_data brackets" % (path, module))
        image += int(end.group(1), 16) - int(start.group(1), 16)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("host", "map"))
    parser.add_argument("--root", required=True, help="the source tree")
    parser.add_argument("--map", help="the linker map (map mode)")
    args = parser.parse_args()

    items = fixed_demand(args.root)
    if args.mode == "map":
        if not args.map:
            fail("map mode needs --map")
        image = map_image(args.map)
        if image > IMAGE_ALLOWANCE_KB * 1024:
            fail("%s: the image takes %d KiB, over the %d KiB allowance: raise IMAGE_ALLOWANCE_KB "
                 "and the SIZE minimum in src/mac9/mac_resources.r together"
                 % (args.map, image // 1024, IMAGE_ALLOWANCE_KB))
        items.append(("image (.text, .data, .bss, module snapshots)", image))
    else:
        items.append(("image allowance", IMAGE_ALLOWANCE_KB * 1024))
    minimum, preferred = size_minimum(args.root)

    demand = sum(size for _, size in items)
    for item, size in items:
        print("%-52s %9d KiB" % (item, (size + 1023) // 1024))
    print("%-52s %9d KiB" % ("fixed demand", (demand + 1023) // 1024))
    print("%-52s %9d KiB (preferred %d KiB)" % ("SIZE minimum", minimum // 1024, preferred // 1024))
    print("%-52s %9d KiB (at least %d KiB)" % ("headroom at the minimum", (minimum - demand) // 1024, HEADROOM_KB))
    if minimum - demand < HEADROOM_KB * 1024:
        fail("the SIZE minimum in src/mac9/mac_resources.r leaves %d KiB beside the fixed demand, "
             "under the %d KiB headroom " % ((minimum - demand) // 1024, HEADROOM_KB))

    # Com_InitHunkMemory clamps the hunk to MaxBlock() less this reserve. The
    # zone, small zone, stack and image are already allocated by then, so at
    # the minimum the hunk sees the minimum less them.
    reserve = hunk_reserve(args.root)
    sound = next(size for item, size in items if item.startswith("sound pool"))
    print("%-52s %9d KiB (needs %d KiB)"
          % ("hunk reserve (MAC_HUNK_RESERVE_KB)", reserve // 1024, (sound + 1023) // 1024 + RESERVE_ITEMS_KB))
    if reserve < sound + RESERVE_ITEMS_KB * 1024:
        fail("MAC_HUNK_RESERVE_KB in src/qcommon/common.c (%d KiB) does not hold the sound pool and "
             "the %d KiB it is reserved for" % (reserve // 1024, RESERVE_ITEMS_KB))
    if minimum - demand + sound < reserve:
        fail("at the SIZE minimum, MAC_HUNK_RESERVE_KB in src/qcommon/common.c (%d KiB) leaves no room "
             "for the default hunk, which Com_InitHunkMemory would shrink" % (reserve // 1024))


if __name__ == "__main__":
    main()
