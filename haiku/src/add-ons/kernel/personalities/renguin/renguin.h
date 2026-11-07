/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGUIN_H
#define _RENGUIN_H


#include <SupportDefs.h>

#include <foreign_personality.h>


// Where the Linux userland lives (the "emul path", FreeBSD's /compat/linux).
// Absolute Linux paths are looked up here first and fall back to Haiku's own
// tree, as FreeBSD's linux_emul_convpath() did. Haiku's "/" is an in-memory
// rootfs, so the tree lives on the boot volume.
#define RENGUIN_LINUX_ROOT	"/boot/system/non-packaged/compat/linux"

// Linux ELF constants (ELF64, x86-64).
#define RENGUIN_EI_CLASS		4
#define RENGUIN_EI_DATA			5
#define RENGUIN_EI_OSABI		7
#define RENGUIN_ELFCLASS64		2
#define RENGUIN_ELFDATA2LSB		1
#define RENGUIN_ELFOSABI_SYSV	0
#define RENGUIN_ELFOSABI_LINUX	3
#define RENGUIN_ELFOSABI_GNU	3
#define RENGUIN_ET_EXEC			2
#define RENGUIN_ET_DYN			3
#define RENGUIN_EM_X86_64		62

#define RENGUIN_PT_LOAD			1
#define RENGUIN_PT_DYNAMIC		2
#define RENGUIN_PT_INTERP		3
#define RENGUIN_PT_NOTE			4
#define RENGUIN_PT_PHDR			6
#define RENGUIN_PT_TLS			7
#define RENGUIN_PT_GNU_STACK	0x6474e551

#define RENGUIN_PF_X			0x1
#define RENGUIN_PF_W			0x2
#define RENGUIN_PF_R			0x4

#define RENGUIN_NT_GNU_ABI_TAG	1
#define RENGUIN_GNU_ABI_LINUX	0

// Linux auxiliary vector entry types (a_type).
#define RENGUIN_AT_NULL			0
#define RENGUIN_AT_IGNORE		1
#define RENGUIN_AT_PHDR			3
#define RENGUIN_AT_PHENT		4
#define RENGUIN_AT_PHNUM		5
#define RENGUIN_AT_PAGESZ		6
#define RENGUIN_AT_BASE			7
#define RENGUIN_AT_FLAGS		8
#define RENGUIN_AT_ENTRY		9
#define RENGUIN_AT_UID			11
#define RENGUIN_AT_EUID			12
#define RENGUIN_AT_GID			13
#define RENGUIN_AT_EGID			14
#define RENGUIN_AT_PLATFORM		15
#define RENGUIN_AT_HWCAP		16
#define RENGUIN_AT_CLKTCK		17
#define RENGUIN_AT_SECURE		23
#define RENGUIN_AT_RANDOM		25
#define RENGUIN_AT_HWCAP2		26
#define RENGUIN_AT_EXECFN		31

#define RENGUIN_AT_RANDOM_LEN	16

// Linux errno values that handlers return directly. The full native->Linux
// table is in abi/linux_errno.h / abi/linux_errno_table.h.
#define RENGUIN_ENOSYS			38


typedef struct {
	uint8	e_ident[16];
	uint16	e_type;
	uint16	e_machine;
	uint32	e_version;
	uint64	e_entry;
	uint64	e_phoff;
	uint64	e_shoff;
	uint32	e_flags;
	uint16	e_ehsize;
	uint16	e_phentsize;
	uint16	e_phnum;
	uint16	e_shentsize;
	uint16	e_shnum;
	uint16	e_shstrndx;
} renguin_ehdr;

typedef struct {
	uint32	p_type;
	uint32	p_flags;
	uint64	p_offset;
	uint64	p_vaddr;
	uint64	p_paddr;
	uint64	p_filesz;
	uint64	p_memsz;
	uint64	p_align;
} renguin_phdr;


#define RENGUIN_BRK_AREA_LOOKUP	(-2)
	// the heap area exists but its ID is not known yet (fork()ed child)


// Per-team data owned by the personality, hung off Team::personality_data.
struct renguin_team {
	addr_t	executable_base;	// load bias of the main executable
	addr_t	interpreter_base;	// load bias of the ELF interpreter, or 0
	addr_t	entry;				// executable entry point (biased)
	addr_t	interpreter_entry;	// interpreter entry point (biased), or 0
	addr_t	phdr;				// address of the executable's program headers
	uint16	phentsize;
	uint16	phnum;
	addr_t	brk_start;			// base of the program break (heap)
	addr_t	brk_current;		// current program break (exact, unaligned)
	addr_t	brk_mapped_end;		// end of the pages backing the break
	area_id	brk_area;			// anonymous area backing the heap, or -1;
								// RENGUIN_BRK_AREA_LOOKUP after fork()
	addr_t	clear_child_tid;	// set_tid_address(): zero + futex on exit
	addr_t	set_child_tid;		// clone(CLONE_CHILD_SETTID) in a fork()ed
								// child: written when it first runs
	uint64	fork_gs_base;		// parent's GS base, for a fork()ed child
								// (Haiku's fork copies only the FS base)
	image_id app_image;			// registered B_APP_IMAGE, or -1
	bool	have_linux_root;	// RENGUIN_LINUX_ROOT existed at exec time
	uint32	umask;				// file creation mask. Haiku's kernel does not
								// apply one (libroot does, in userland), so
								// Renguin applies it for Linux programs
	char	exe_path[B_PATH_NAME_LENGTH];
		// host path of the executable (for /proc/self/exe)
};


#ifdef __cplusplus
extern "C" {
#endif

// errno.cpp
int		renguin_errno_from_haiku(status_t status);

// Encode a Haiku result for return in rax: a byte count or value stays,
// a negative Haiku status_t becomes a negative Linux errno.
static inline int64
renguin_encode(ssize_t result)
{
	if (result < 0)
		return -(int64)renguin_errno_from_haiku((status_t)result);
	return result;
}

// loader.cpp
status_t	renguin_probe(const struct foreign_exec_args* args);
status_t	renguin_load(struct foreign_exec_args* args);
status_t	renguin_enter_userspace(Thread* thread, addr_t entry,
				addr_t stackPointer);
void		renguin_team_released(Team* team);
bool		renguin_is_linux_team(Team* team);

// exec.cpp
status_t	renguin_team_forked(Team* parent, Team* child);
void		renguin_thread_entering_userspace(Thread* thread, bool forked);
int64		renguin_sys_clone(uint64 flags, uint64 newStack, uint64 parentTid,
				uint64 childTid, uint64 tls);
int64		renguin_sys_fork();
int64		renguin_sys_vfork();
int64		renguin_sys_wait4(int64 pid, uint64 status, uint64 options,
				uint64 rusage);
int64		renguin_sys_execve(uint64 path, uint64 argv, uint64 envp);

// syscall.cpp
void		renguin_syscall(struct iframe* frame);
struct renguin_team* renguin_current_team();

// usermem.cpp: checked copies to and from the Linux program's memory. Unlike
// a bare user_memcpy(), these refuse kernel addresses. They return B_OK or
// B_BAD_ADDRESS.
status_t	renguin_copy_in(void* to, uint64 userFrom, size_t size);
status_t	renguin_copy_out(uint64 userTo, const void* from, size_t size);
ssize_t		renguin_copy_string_in(char* to, uint64 userFrom, size_t size);
	// returns the length, B_NAME_TOO_LONG, or B_BAD_ADDRESS

// usermem.cpp: scratch space in the calling thread's user stack, below the
// System V red zone, for passing data to Haiku's _user_* calls (which only
// accept user addresses). Valid until the syscall returns.
struct renguin_scratch {
	addr_t	next;
};
void		renguin_scratch_init(struct renguin_scratch* scratch);
uint64		renguin_scratch_alloc(struct renguin_scratch* scratch, size_t size);
uint64		renguin_scratch_put(struct renguin_scratch* scratch,
				const void* data, size_t size);
	// returns 0 if the user stack is not writable

// path.cpp: Linux root ("emul path") handling.
#define RENGUIN_PATH_FOLLOW_LEAF	0x1		// resolve a final symlink
#define RENGUIN_PATH_MAY_CREATE		0x2		// final component may not exist
status_t	renguin_resolve_in_root(const char* linuxPath, uint32 flags,
				char* hostPath, size_t hostPathSize);
	// B_OK if the path exists under the Linux root; hostPath is then the
	// fully resolved host path. B_ENTRY_NOT_FOUND otherwise.
status_t	renguin_get_user_path(uint64 userPath, uint32 flags, char* path,
				size_t pathSize);
	// copies a path from userland and translates absolute paths into the
	// Linux root where they exist there
uint64		renguin_user_path(struct renguin_scratch* scratch, uint64 userPath,
				uint32 flags, status_t* _status);
	// like renguin_get_user_path(), but returns a user-space copy of the
	// translated path for Haiku's _user_* calls (or userPath unchanged)
void		renguin_host_to_linux_path(char* path);
	// strips the Linux root prefix from a host path, in place

// mem.cpp
int64		renguin_sys_brk(uint64 addr);
int64		renguin_sys_mmap(uint64 addr, uint64 length, uint64 prot,
				uint64 flags, int64 fd, int64 offset);
int64		renguin_sys_munmap(uint64 addr, uint64 length);
int64		renguin_sys_mprotect(uint64 addr, uint64 length, uint64 prot);
int64		renguin_sys_mremap(uint64 oldAddr, uint64 oldSize, uint64 newSize,
				uint64 flags, uint64 newAddr);
int64		renguin_sys_msync(uint64 addr, uint64 length, uint64 flags);
int64		renguin_sys_madvise(uint64 addr, uint64 length, uint64 advice);

// file.cpp
uint32		renguin_current_umask();
int64		renguin_sys_read(int fd, uint64 buffer, uint64 count);
int64		renguin_sys_write(int fd, uint64 buffer, uint64 count);
int64		renguin_sys_pread64(int fd, uint64 buffer, uint64 count,
				int64 offset);
int64		renguin_sys_pwrite64(int fd, uint64 buffer, uint64 count,
				int64 offset);
int64		renguin_sys_writev(int fd, uint64 iov, uint64 count);
int64		renguin_sys_readv(int fd, uint64 iov, uint64 count);
int64		renguin_sys_openat(int dirfd, uint64 path, uint64 flags, uint64 mode);
int64		renguin_sys_close(int fd);
int64		renguin_sys_lseek(int fd, int64 offset, int whence);
int64		renguin_sys_fstat(int fd, uint64 statbuf);
int64		renguin_sys_newfstatat(int dirfd, uint64 path, uint64 statbuf,
				uint64 flags);
int64		renguin_sys_readlinkat(int dirfd, uint64 path, uint64 buffer,
				uint64 size);
int64		renguin_sys_faccessat(int dirfd, uint64 path, uint64 mode,
				uint64 flags);
int64		renguin_sys_getdents64(int fd, uint64 buffer, uint64 count);
int64		renguin_sys_dup(int fd);
int64		renguin_sys_dup3(int oldfd, int newfd, uint64 flags, bool isDup2);
int64		renguin_sys_pipe2(uint64 fds, uint64 flags);
int64		renguin_sys_fcntl(int fd, uint64 cmd, uint64 arg);
int64		renguin_sys_getcwd(uint64 buffer, uint64 size);
int64		renguin_sys_chdir(uint64 path);
int64		renguin_sys_fchdir(int fd);
int64		renguin_sys_mkdirat(int dirfd, uint64 path, uint64 mode);
int64		renguin_sys_unlinkat(int dirfd, uint64 path, uint64 flags);
int64		renguin_sys_renameat(int olddirfd, uint64 oldpath, int newdirfd,
				uint64 newpath);
int64		renguin_sys_fsync(int fd);
int64		renguin_sys_ftruncate(int fd, int64 length);

// tty.cpp
int64		renguin_sys_ioctl(int fd, uint64 cmd, uint64 arg);

// misc.cpp
int64		renguin_sys_uname(uint64 buffer);
int64		renguin_sys_getrandom(uint64 buffer, uint64 length, uint64 flags);
int64		renguin_sys_set_tid_address(uint64 tidptr);
int64		renguin_sys_clock_gettime(uint64 clockId, uint64 timespec);
int64		renguin_sys_clock_getres(uint64 clockId, uint64 timespec);
int64		renguin_sys_gettimeofday(uint64 timeval, uint64 timezone);
int64		renguin_sys_time(uint64 tloc);
int64		renguin_sys_nanosleep(uint64 request, uint64 remain);
int64		renguin_sys_clock_nanosleep(uint64 clockId, uint64 flags,
				uint64 request, uint64 remain);
int64		renguin_sys_arch_prctl(uint64 code, uint64 addr);
int64		renguin_sys_prctl(uint64 option, uint64 arg2);
int64		renguin_sys_sched_yield();

// process.cpp
int64		renguin_sys_getpid();
int64		renguin_sys_gettid();
int64		renguin_sys_getppid();
int64		renguin_sys_getpgid(int64 pid);
int64		renguin_sys_getsid(int64 pid);
int64		renguin_sys_setpgid(int64 pid, int64 pgid);
int64		renguin_sys_setsid();
int64		renguin_sys_getuid();
int64		renguin_sys_geteuid();
int64		renguin_sys_getgid();
int64		renguin_sys_getegid();
int64		renguin_sys_getresuid(uint64 ruid, uint64 euid, uint64 suid);
int64		renguin_sys_getresgid(uint64 rgid, uint64 egid, uint64 sgid);
int64		renguin_sys_kill(int64 pid, int64 signal);
int64		renguin_sys_tgkill(int64 tgid, int64 tid, int64 signal);
int64		renguin_sys_prlimit64(int64 pid, uint64 resource, uint64 newLimit,
				uint64 oldLimit);
int64		renguin_sys_umask(uint64 mask);
int64		renguin_sys_rt_sigaction(uint64 signal, uint64 action,
				uint64 oldAction, uint64 sigsetSize);
int64		renguin_sys_rt_sigprocmask(uint64 how, uint64 set, uint64 oldSet,
				uint64 sigsetSize);
int64		renguin_sys_sigaltstack(uint64 stack, uint64 oldStack);
int			renguin_signal_to_haiku(int linuxSignal);
int			renguin_signal_from_haiku(int haikuSignal);

#ifdef __cplusplus
}
#endif


#endif	// _RENGUIN_H
