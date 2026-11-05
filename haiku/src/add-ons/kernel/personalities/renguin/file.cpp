/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux file and descriptor system calls: read, write, readv, writev, openat,
 * close, lseek, fstat, newfstatat, readlinkat.
 *
 * These wrap Haiku's _user_* VFS calls, translating Linux open flags, the
 * struct stat layout, and *at() directory-fd conventions. Linux and Haiku both
 * use AT_FDCWD == -100, so directory fds pass through unchanged.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <KernelExport.h>

#include <sys/stat.h>

#include <vfs.h>


static int
open_flags_to_haiku(uint64 flags)
{
	int result = (int)(flags & LX_O_ACCMODE);	// RDONLY/WRONLY/RDWR match
	if ((flags & LX_O_CREAT) != 0)
		result |= O_CREAT;
	if ((flags & LX_O_EXCL) != 0)
		result |= O_EXCL;
	if ((flags & LX_O_TRUNC) != 0)
		result |= O_TRUNC;
	if ((flags & LX_O_APPEND) != 0)
		result |= O_APPEND;
	if ((flags & LX_O_NONBLOCK) != 0)
		result |= O_NONBLOCK;
	if ((flags & LX_O_CLOEXEC) != 0)
		result |= O_CLOEXEC;
	if ((flags & LX_O_DIRECTORY) != 0)
		result |= O_DIRECTORY;
	if ((flags & LX_O_NOFOLLOW) != 0)
		result |= O_NOFOLLOW;
	if ((flags & LX_O_NOCTTY) != 0)
		result |= O_NOCTTY;
	return result;
}


//! Translate a Haiku struct stat into the Linux x86-64 layout.
static void
stat_to_linux(const struct stat& source, linux_stat& dest)
{
	memset(&dest, 0, sizeof(dest));
	dest.st_dev = source.st_dev;
	dest.st_ino = source.st_ino;
	dest.st_nlink = source.st_nlink;
	dest.st_mode = source.st_mode;
	dest.st_uid = source.st_uid;
	dest.st_gid = source.st_gid;
	dest.st_rdev = source.st_rdev;
	dest.st_size = source.st_size;
	dest.st_blksize = source.st_blksize;
	dest.st_blocks = source.st_blocks;
	dest.st_atime_sec = source.st_atim.tv_sec;
	dest.st_atime_nsec = source.st_atim.tv_nsec;
	dest.st_mtime_sec = source.st_mtim.tv_sec;
	dest.st_mtime_nsec = source.st_mtim.tv_nsec;
	dest.st_ctime_sec = source.st_ctim.tv_sec;
	dest.st_ctime_nsec = source.st_ctim.tv_nsec;
}


int64
renguin_sys_read(int fd, uint64 buffer, uint64 count)
{
	return renguin_encode(_user_read(fd, -1, (void*)buffer, (size_t)count));
}


int64
renguin_sys_write(int fd, uint64 buffer, uint64 count)
{
	return renguin_encode(_user_write(fd, -1, (const void*)buffer,
		(size_t)count));
}


//! Linux readv/writev: loop over the iovec in user memory, one _user_* call
//! each. Partial progress is returned, matching Linux.
static int64
do_vector_io(int fd, uint64 iov, uint64 count, bool write)
{
	if ((int64)count < 0)
		return -LX_EINVAL;

	int64 total = 0;
	for (uint64 i = 0; i < count; i++) {
		linux_iovec vec;
		if (user_memcpy(&vec, (void*)(iov + i * sizeof(linux_iovec)),
				sizeof(vec)) != B_OK) {
			return total > 0 ? total : -LX_EFAULT;
		}
		if (vec.iov_len == 0)
			continue;

		ssize_t done = write
			? _user_write(fd, -1, (const void*)vec.iov_base, vec.iov_len)
			: _user_read(fd, -1, (void*)vec.iov_base, vec.iov_len);
		if (done < 0)
			return total > 0 ? total : -(int64)renguin_errno_from_haiku(done);
		total += done;
		if ((uint64)done < vec.iov_len)
			break;		// short read/write ends the sequence
	}
	return total;
}


int64
renguin_sys_writev(int fd, uint64 iov, uint64 count)
{
	return do_vector_io(fd, iov, count, true);
}


int64
renguin_sys_readv(int fd, uint64 iov, uint64 count)
{
	return do_vector_io(fd, iov, count, false);
}


int64
renguin_sys_openat(int dirfd, uint64 path, uint64 flags, uint64 mode)
{
	// Haiku and Linux share AT_FDCWD == -100, so dirfd passes through.
	int result = _user_open(dirfd, (const char*)path,
		open_flags_to_haiku(flags), (int)mode);
	if (result < 0)
		return -(int64)renguin_errno_from_haiku(result);
	return result;
}


int64
renguin_sys_close(int fd)
{
	return renguin_encode(_user_close(fd));
}


int64
renguin_sys_lseek(int fd, int64 offset, int whence)
{
	// Linux SEEK_SET/CUR/END (0/1/2) match Haiku's.
	off_t result = _user_seek(fd, offset, whence);
	if (result < 0)
		return -(int64)renguin_errno_from_haiku((status_t)result);
	return result;
}


int64
renguin_sys_fstat(int fd, uint64 statbuf)
{
	struct stat haikuStat;
	status_t status = _user_read_stat(fd, NULL, false, &haikuStat,
		sizeof(haikuStat));
	if (status != B_OK)
		return -(int64)renguin_errno_from_haiku(status);

	linux_stat out;
	stat_to_linux(haikuStat, out);
	if (user_memcpy((void*)statbuf, &out, sizeof(out)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_newfstatat(int dirfd, uint64 path, uint64 statbuf, uint64 flags)
{
	bool traverse = (flags & LX_AT_SYMLINK_NOFOLLOW) == 0;

	// AT_EMPTY_PATH with an empty path means fstat(dirfd); Haiku expresses
	// that as a NULL path with the fd.
	const char* pathArg = (const char*)path;
	if ((flags & LX_AT_EMPTY_PATH) != 0)
		pathArg = NULL;

	struct stat haikuStat;
	status_t status = _user_read_stat(dirfd, pathArg, traverse, &haikuStat,
		sizeof(haikuStat));
	if (status != B_OK)
		return -(int64)renguin_errno_from_haiku(status);

	linux_stat out;
	stat_to_linux(haikuStat, out);
	if (user_memcpy((void*)statbuf, &out, sizeof(out)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_readlinkat(int dirfd, uint64 path, uint64 buffer, uint64 size)
{
	if ((int64)size <= 0)
		return -LX_EINVAL;

	size_t bufferSize = (size_t)size;
	status_t status = _user_read_link(dirfd, (const char*)path,
		(char*)buffer, &bufferSize);
	if (status != B_OK)
		return -(int64)renguin_errno_from_haiku(status);

	// Linux returns the number of bytes placed (never NUL-terminated), capped
	// at the buffer size. Haiku returns the full length in bufferSize.
	return bufferSize < (size_t)size ? (int64)bufferSize : (int64)size;
}
