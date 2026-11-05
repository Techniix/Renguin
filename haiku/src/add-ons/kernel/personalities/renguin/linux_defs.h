/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux x86-64 userspace ABI constants and structures used by the syscall
 * handlers. These are stable Linux UAPI numbers (the values a Linux program is
 * compiled against); they are written out here rather than derived from any
 * particular libc so the module has no Linux build dependency.
 */
#ifndef _RENGUIN_LINUX_DEFS_H
#define _RENGUIN_LINUX_DEFS_H


#include <SupportDefs.h>


// Linux errno values returned directly by handlers. The full native->Linux
// map is in errno.cpp / abi/linux_errno_table.h.
#define LX_EPERM		1
#define LX_ENOENT		2
#define LX_EBADF		9
#define LX_ENOMEM		12
#define LX_EFAULT		14
#define LX_EINVAL		22
#define LX_ERANGE		34
#define LX_ENOSYS		38
#define LX_ENOTTY		25

// open()/openat() flags (Linux x86-64).
#define LX_O_RDONLY		00000000
#define LX_O_WRONLY		00000001
#define LX_O_RDWR		00000002
#define LX_O_ACCMODE	00000003
#define LX_O_CREAT		00000100
#define LX_O_EXCL		00000200
#define LX_O_NOCTTY		00000400
#define LX_O_TRUNC		00001000
#define LX_O_APPEND		00002000
#define LX_O_NONBLOCK	00004000
#define LX_O_DIRECTORY	00200000
#define LX_O_NOFOLLOW	00400000
#define LX_O_CLOEXEC	02000000

// *at() special fd and flags.
#define LX_AT_FDCWD				-100
#define LX_AT_SYMLINK_NOFOLLOW	0x100
#define LX_AT_EMPTY_PATH		0x1000

// mmap()/mprotect() flags.
#define LX_PROT_READ	0x1
#define LX_PROT_WRITE	0x2
#define LX_PROT_EXEC	0x4

#define LX_MAP_SHARED		0x01
#define LX_MAP_PRIVATE		0x02
#define LX_MAP_FIXED		0x10
#define LX_MAP_ANONYMOUS	0x20
#define LX_MAP_NORESERVE	0x4000

#define LX_MAP_FAILED		((uint64)-1)

// arch_prctl() subfunctions.
#define LX_ARCH_SET_GS	0x1001
#define LX_ARCH_SET_FS	0x1002
#define LX_ARCH_GET_FS	0x1003
#define LX_ARCH_GET_GS	0x1004

// clock_gettime() clock ids.
#define LX_CLOCK_REALTIME	0
#define LX_CLOCK_MONOTONIC	1

// getrandom() flags.
#define LX_GRND_NONBLOCK	0x0001
#define LX_GRND_RANDOM		0x0002


// Linux x86-64 struct stat. Field order and padding differ from Haiku's, so
// fstat()/newfstatat() translate into this explicitly.
struct linux_stat {
	uint64	st_dev;
	uint64	st_ino;
	uint64	st_nlink;
	uint32	st_mode;
	uint32	st_uid;
	uint32	st_gid;
	uint32	__pad0;
	uint64	st_rdev;
	int64	st_size;
	int64	st_blksize;
	int64	st_blocks;
	int64	st_atime_sec;
	int64	st_atime_nsec;
	int64	st_mtime_sec;
	int64	st_mtime_nsec;
	int64	st_ctime_sec;
	int64	st_ctime_nsec;
	int64	__unused[3];
};

// Linux struct new_utsname: six fixed 65-byte fields.
struct linux_utsname {
	char	sysname[65];
	char	nodename[65];
	char	release[65];
	char	version[65];
	char	machine[65];
	char	domainname[65];
};

// Linux struct timespec (64-bit time_t on x86-64).
struct linux_timespec {
	int64	tv_sec;
	int64	tv_nsec;
};

// Linux struct iovec.
struct linux_iovec {
	uint64	iov_base;
	uint64	iov_len;
};


#endif	// _RENGUIN_LINUX_DEFS_H
