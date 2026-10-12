/* SPDX-License-Identifier: GPL-2.0-or-later
 * tegra-present-probe -- M3i: bounded process resource sampling from /proc.
 *
 * Sampling is deliberately bounded (once per second, a fixed stack buffer, no
 * allocation) and only reads the *process totals*; it never opens device
 * handles and never asserts leak-freedom. A driver may legitimately hold one
 * fd per in-flight fence, so the endpoint fd count is not, by itself, evidence
 * of a leak.
 */
#ifndef TP_PROCSTAT_H
#define TP_PROCSTAT_H

/* Number of entries in /proc/self/fd, excluding the opendir() fd we open to
 * count them. Returns -1 when /proc is unavailable. */
int tp_proc_fd_count(void);

/* Resident set size in KiB: "VmRSS" from /proc/self/status, falling back to
 * /proc/self/statm resident pages * page size. Returns -1 on error. */
int tp_proc_rss_kb(void);

/* Pure parser (no syscalls) for a /proc/self/status line like
 *   "VmRSS:\t    1234 kB"
 * Returns 0 and stores the value when the line is the VmRSS field. */
int tp_parse_vmrss_kb(const char *line, long *kb_out);

#endif /* TP_PROCSTAT_H */
