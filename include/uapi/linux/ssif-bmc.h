/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * Copyright (c) 2020, Ampere Computing LLC.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef _UAPI_LINUX_SSIF_BMC_H
#define _UAPI_LINUX_SSIF_BMC_H

#include <linux/ioctl.h>

#define __SSIF_BMC_IOCTL_MAGIC	0xb1
#define SSIF_BMC_IOCTL_SMS_ATN	_IO(__SSIF_BMC_IOCTL_MAGIC, 0x00)

#endif /* _UAPI_LINUX_SSIF_BMC_H */
