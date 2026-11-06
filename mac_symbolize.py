#!/usr/bin/env python3
"""Turn the code+0xNNNN addresses in a Mac q3boot.log into source lines.

The boot log (code/mac/mac_crash.c) gives crash addresses as offsets into the
application's code section, which are the addresses in the XCOFF the build
leaves next to the PEF. Use the XCOFF from the same build as the log; the log's
first lines name its revision.

    python3 mac_symbolize.py q3boot.log [build_mac/Quake3.xcoff]
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
TOOL = 'powerpc-apple-macos-addr2line'
OFFSET = re.compile(r'code\+0x([0-9a-fA-F]+)')


def find_addr2line():
    bundled = os.path.join(ROOT, 'tools', 'Retro68-build', 'bin', TOOL)
    return os.environ.get('ADDR2LINE') or (bundled if os.path.exists(bundled) else shutil.which(TOOL))


def symbolize(tool, xcoff, offsets):
    """Map each offset to 'function at file:line'."""
    command = [tool, '-f', '-e', xcoff] + ['0x' + offset for offset in offsets]
    lines = subprocess.check_output(command, text=True).splitlines()
    if len(lines) != 2 * len(offsets):
        raise SystemExit('%s gave %d lines for %d addresses' % (tool, len(lines), len(offsets)))
    return {offset: '%s at %s' % (lines[2 * i].lstrip('.'), lines[2 * i + 1])
            for i, offset in enumerate(offsets)}


def main(argv):
    if len(argv) not in (2, 3):
        raise SystemExit(__doc__)
    log = open(argv[1], encoding='mac_roman', newline=None).read()
    xcoff = argv[2] if len(argv) == 3 else os.path.join(ROOT, 'build_mac', 'Quake3.xcoff')
    tool = find_addr2line()
    if not tool:
        raise SystemExit('%s was not found; set ADDR2LINE' % TOOL)
    if not os.path.exists(xcoff):
        raise SystemExit('%s was not found' % xcoff)
    lines = [line for line in log.split('\n') if OFFSET.search(line)]
    if not lines:
        print('No crash addresses in %s.' % argv[1])
        return 0
    offsets = sorted({match for line in lines for match in OFFSET.findall(line)})
    names = symbolize(tool, xcoff, offsets)
    for line in lines:
        print(OFFSET.sub(lambda match: names[match.group(1)], line.split('] ', 1)[-1]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
