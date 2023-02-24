/*
 * Copyright (c) 2017 MediaTek Inc.
 * Author: CJ Yang <cj.yang@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __IVP_DBG_CTRL_H__
#define __IVP_DBG_CTRL_H__

/* debug level */
#define DBG_LEVEL_ERROR		1
#define DBG_LEVEL_WARNING	2
#define DBG_LEVEL_INFO		4
#define DBG_LEVEL_PROC		8
#define DBG_LEVEL_MAX		(DBG_LEVEL_ERROR | DBG_LEVEL_WARNING |	\
				DBG_LEVEL_INFO | DBG_LEVEL_PROC)

/* ptrace level */
#define PTRACE_LEVEL_MAIN		0x00000001
#define PTRACE_LEVEL_USER_DEF01		0x00000002
#define PTRACE_LEVEL_USER_DEF02		0x00000004
#define PTRACE_LEVEL_USER_DEF03		0x00000008
#define PTRACE_LEVEL_USER_DEF04		0x00000010
#define PTRACE_LEVEL_USER_DEF05		0x00000020
#define PTRACE_LEVEL_USER_DEF06		0x00000040
#define PTRACE_LEVEL_USER_DEF07		0x00000080
#define PTRACE_LEVEL_USER_DEF08		0x00000100
#define PTRACE_LEVEL_USER_DEF09		0x00000200
#define PTRACE_LEVEL_USER_DEF10		0x00000400
#define PTRACE_LEVEL_USER_DEF11		0x00000800
#define PTRACE_LEVEL_USER_DEF12		0x00001000
#define PTRACE_LEVEL_USER_DEF13		0x00002000
#define PTRACE_LEVEL_USER_DEF14		0x00004000
#define PTRACE_LEVEL_USER_DEF15		0x00008000
#define PTRACE_LEVEL_USER_DEF16		0x00010000
#define PTRACE_LEVEL_USER_DEF17		0x00020000
#define PTRACE_LEVEL_USER_DEF18		0x00040000
#define PTRACE_LEVEL_USER_DEF19		0x00080000
#define PTRACE_LEVEL_USER_DEF20		0x00100000
#define PTRACE_LEVEL_USER_DEF21		0x00200000
#define PTRACE_LEVEL_USER_DEF22		0x00400000
#define PTRACE_LEVEL_USER_DEF23		0x00800000
#define PTRACE_LEVEL_USER_DEF24		0x01000000
#define PTRACE_LEVEL_USER_DEF25		0x02000000
#define PTRACE_LEVEL_USER_DEF26		0x04000000
#define PTRACE_LEVEL_USER_DEF27		0x08000000
#define PTRACE_LEVEL_USER_DEF28		0x10000000
#define PTRACE_LEVEL_USER_DEF29		0x20000000
#define PTRACE_LEVEL_USER_DEF30		0x40000000
#define PTRACE_LEVEL_USER_DEF31		0x80000000
#define PTRACE_LEVEL_MAX		0xFFFFFFFF

/* debug log control */
#define DBG_CTRL_LOG_BUFFER		1
#define DBG_CTRL_LOG_UART		2
#define DBG_CTRL_LOG_BUFFER_RING	4
#define DBG_CTRL_MAX		(DBG_CTRL_LOG_BUFFER | DBG_CTRL_LOG_UART | \
				 DBG_CTRL_LOG_BUFFER_RING)

#endif  /* __IVP_DBG_CTRL_H__ */

