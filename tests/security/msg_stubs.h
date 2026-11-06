/* Just enough engine for msg.c and huffman.c in a sanitizer build. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
extern jmp_buf test_drop;
extern int test_drops;
