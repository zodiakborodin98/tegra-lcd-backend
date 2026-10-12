/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tp.h"

#include <stdio.h>
#include <stdarg.h>

static int g_verbose = 0;

void tp_set_verbose(int v) { g_verbose = v; }

void tp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void tp_vlog(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
