/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "procstat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>

int tp_proc_fd_count(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        n++;
    }
    closedir(d);
    /* The opendir() fd itself is one of the counted entries. */
    return n - 1;
}

int tp_parse_vmrss_kb(const char *line, long *kb_out)
{
    if (!line)
        return -1;
    const char *p = line;
    if (strncmp(p, "VmRSS:", 6) != 0)
        return -1;
    p += 6;
    while (*p == ' ' || *p == '\t')
        p++;
    char *end = NULL;
    errno = 0;
    long v = strtol(p, &end, 10);
    if (errno != 0 || end == p || v < 0)
        return -1;
    if (kb_out)
        *kb_out = v;
    return 0;
}

int tp_proc_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        long kb = 0;
        while (fgets(line, sizeof(line), f) != NULL) {
            if (tp_parse_vmrss_kb(line, &kb) == 0) {
                fclose(f);
                return (int)kb;
            }
        }
        fclose(f);
    }

    /* Fallback: /proc/self/statm: size resident shared text lib data dt (pages). */
    f = fopen("/proc/self/statm", "r");
    if (f) {
        long size = 0, res = 0;
        int rc = fscanf(f, "%ld %ld", &size, &res);
        fclose(f);
        if (rc == 2 && size >= 0 && res >= 0) {
            long ps = sysconf(_SC_PAGESIZE);
            if (ps <= 0)
                ps = 4096;
            return (int)((res * ps) / 1024);
        }
    }
    return -1;
}
