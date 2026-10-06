#!/bin/sh
# build-cdi4dc.sh -- a Linux cdi4dc (SiZiOUS's img4dc), for mkdisc.sh's CDI=1:
#
#   dreamcast/tools/build-cdi4dc.sh [dir]     # default ~/build/tools/dc/img4dc
#
# img4dc is written for Windows (TDM-GCC, CodeLite): its console code calls
# the Win32 console API and its CDI writer assumes a 32-bit long. This clones
# it, swaps the console code for a plain stdio one, makes its longs ints and
# builds <dir>/cdi4dc/cdi4dc with the host gcc. Nothing of it is committed here.
set -eu
dir=${1:-$HOME/build/tools/dc/img4dc}
rev=20d1c1489102bef780a4e9bbd888c258acd6d1e4

[ -d "$dir/.git" ] || git -c core.autocrlf=false clone -q https://github.com/sizious/img4dc "$dir"
cd "$dir"
git -c core.autocrlf=false checkout -q -f "$rev"

# patch.h declares bcopy, which glibc has (the same, memmove's argument order).
sed -i 's/^void bcopy(unsigned char \*src, unsigned char \*dest, int len);/#ifdef _WIN32\n&\n#endif/' edc/inc/patch.h
# The CDI header's fields are written with sizeof: 4 bytes on Win32, 8 here.
sed -i -E 's/\blong\b/int/g' cdi4dc/src/*.c cdi4dc/inc/*.h

mkdir -p posix
cat > posix/console.h <<'EOF'
/* stdio stand-in for common/inc/console.h (the Win32 console). */
#ifndef __CONSOLE_H__
#define __CONSOLE_H__
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIGHT_GRAY 7
#define LIGHT_RED 12
char *get_program_name(void);
void set_program_name(char *argv0);
void textColor(int color);
void gotoXY(int x, int y);
int whereX(void);
int whereY(void);
void get_full_filename(char *in, char *result);
unsigned int fsize(FILE *f);
#define printf_colored(c, ...) printf(__VA_ARGS__)
#define printf_stderr_colored(c, ...) fprintf(stderr, __VA_ARGS__)
#endif
EOF
cat > posix/console.c <<'EOF'
/* stdio stand-in for common/src/console.c: no colours, no cursor moves. */
#include <libgen.h>
#include <limits.h>
#include "console.h"
static char program_name[256] = "cdi4dc";
char *get_program_name(void) { return program_name; }
void set_program_name(char *argv0) {
    char t[PATH_MAX]; snprintf(t, sizeof t, "%s", argv0);
    snprintf(program_name, sizeof program_name, "%s", basename(t));
}
void textColor(int color) { (void)color; }
void gotoXY(int x, int y) { (void)x; (void)y; fputc('\r', stdout); fflush(stdout); }
int whereX(void) { return 0; }
int whereY(void) { return 0; }
void get_full_filename(char *in, char *result) {
    char b[PATH_MAX];
    if (realpath(in, b)) strcpy(result, b); else if (result != in) strcpy(result, in);
}
/* tools.h declares it (as fsize_stream in common.c); the Windows build has it elsewhere. */
unsigned int fsize(FILE *f) {
    long cur = ftell(f), len;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, cur, SEEK_SET);
    return (unsigned int)len;
}
EOF
gcc -O2 -w -Dfsize_stream=fsize -Iposix -Icdi4dc/inc -Iedc/inc -o cdi4dc/cdi4dc \
    posix/console.c cdi4dc/src/*.c edc/src/edc_ecc.c edc/src/libedc.c
echo "$dir/cdi4dc/cdi4dc"
