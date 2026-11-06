#!/usr/bin/env python3
"""Write a no-op for each function pointer qgl.h declares (argument: output header)."""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
text = open(os.path.join(ROOT, 'src/renderer/qgl.h'), encoding='latin-1').read()
names = set(re.findall(r'\(\s*APIENTRY\s*\*\s*(qgl\w+)\s*\)', text)) | set(re.findall(r'\(\s*\*\s*(qglX\w+)\s*\)', text))
# defined in tr_init.c
EXT = set(re.findall(r'^\S.*\(\s*APIENTRY\s*\*\s*(qgl\w+)\s*\)', open(os.path.join(ROOT, 'src/renderer/tr_init.c'), encoding='latin-1').read(), re.M))
with open(sys.argv[1], 'w') as out:
    out.write('static long gl_noop( void ) { return 0; }\n'
              'static const char *gl_string( unsigned e ) { return e == 0x1F02 ? "1.2.0" : e == 0x1F03 ? "GL_EXT_texture_env_add" : "Stub"; }\n'
              'static void gl_integer( unsigned e, int *v ) { *v = e == 0x0D33 || e == 0x0D34 ? 2048 : 8; }\n')
    specials = {'qglGetString': 'gl_string', 'qglGetIntegerv': 'gl_integer'}
    for n in sorted(names):
        if n not in EXT:
            out.write('void *%s = (void *)%s;\n' % (n, specials.get(n, 'gl_noop')))
