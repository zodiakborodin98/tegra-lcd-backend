/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal userspace shim for tegra-present-probe.
 *
 * The pinned kernel uapi headers (tegra_dc_ext.h, nvhost_ioctl.h) include
 * <uapi/linux/types.h>, which is not present in an extracted/installed
 * sysroot include path. Redirect it to the installed <linux/types.h> and
 * provide the __user/__packed/__bitwise annotations those headers expect.
 * This file contains no kernel implementation, only build glue.
 */
#ifndef TP_UAPI_LINUX_TYPES_SHIM_H
#define TP_UAPI_LINUX_TYPES_SHIM_H
#include <linux/types.h>
#ifndef __user
#define __user
#endif
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __bitwise
#define __bitwise
#endif
#endif
