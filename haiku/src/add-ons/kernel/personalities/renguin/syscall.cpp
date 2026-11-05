/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux x86-64 system call dispatch.
 *
 * The kernel's system call entry (the foreign-ABI branch in
 * src/system/kernel/arch/x86/64/interrupts.S) calls renguin_syscall() with the
 * thread's iframe whenever a thread of a Linux team makes a system call. The
 * call number is in orig_rax and the arguments are in the Linux registers
 * (rdi, rsi, rdx, r10, r8, r9). The result, or a negative Linux errno, goes
 * back in rax.
 *
 * Handlers live in file.cpp, mem.cpp and misc.cpp; this file routes to them.
 * Anything not yet implemented returns -ENOSYS and is logged. The prioritized
 * matrix in the research report (section 12) is the implementation order.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <KernelExport.h>

#include <arch/x86/64/iframe.h>
#include <team.h>
#include <thread.h>

#include "abi/linux_syscall_names.h"


//#define TRACE_RENGUIN_SYSCALL
#ifdef TRACE_RENGUIN_SYSCALL
#	define TRACE(x...) dprintf("renguin syscall: " x)
#else
#	define TRACE(x...) ;
#endif


// Linux x86-64 system call numbers.
enum {
	LINUX_read				= 0,
	LINUX_write				= 1,
	LINUX_close				= 3,
	LINUX_fstat				= 5,
	LINUX_lseek				= 8,
	LINUX_mmap				= 9,
	LINUX_mprotect			= 10,
	LINUX_munmap			= 11,
	LINUX_brk				= 12,
	LINUX_rt_sigaction		= 13,
	LINUX_rt_sigprocmask	= 14,
	LINUX_ioctl				= 16,
	LINUX_readv				= 19,
	LINUX_writev			= 20,
	LINUX_nanosleep			= 35,
	LINUX_exit				= 60,
	LINUX_uname				= 63,
	LINUX_arch_prctl		= 158,
	LINUX_set_tid_address	= 218,
	LINUX_clock_gettime		= 228,
	LINUX_exit_group		= 231,
	LINUX_openat			= 257,
	LINUX_newfstatat		= 262,
	LINUX_readlinkat		= 267,
	LINUX_set_robust_list	= 273,
	LINUX_getrandom			= 318,
	LINUX_rseq				= 334,
};


struct renguin_team*
renguin_current_team()
{
	return (renguin_team*)thread_get_current_thread()->team->personality_data;
}


static void
log_unimplemented(uint64 number)
{
	const char* name = number < LINUX_SYSCALL_COUNT
		? kLinuxSyscallNames[number] : NULL;
	dprintf("renguin: team %" B_PRId32 ": unimplemented Linux syscall %"
		B_PRIu64 " (%s)\n", thread_get_current_thread()->team->id, number,
		name != NULL ? name : "?");
}


void
renguin_syscall(struct iframe* frame)
{
	uint64 number = frame->orig_rax;
	uint64 a1 = frame->di, a2 = frame->si, a3 = frame->dx;
	uint64 a4 = frame->r10, a5 = frame->r8, a6 = frame->r9;
	int64 result;

	TRACE("team %" B_PRId32 " call %" B_PRIu64 "\n",
		thread_get_current_thread()->team->id, number);

	switch (number) {
		// --- I/O and files (file.cpp) ---
		case LINUX_read:
			result = renguin_sys_read((int)a1, a2, a3);
			break;
		case LINUX_write:
			result = renguin_sys_write((int)a1, a2, a3);
			break;
		case LINUX_readv:
			result = renguin_sys_readv((int)a1, a2, a3);
			break;
		case LINUX_writev:
			result = renguin_sys_writev((int)a1, a2, a3);
			break;
		case LINUX_openat:
			result = renguin_sys_openat((int)a1, a2, a3, a4);
			break;
		case LINUX_close:
			result = renguin_sys_close((int)a1);
			break;
		case LINUX_lseek:
			result = renguin_sys_lseek((int)a1, (int64)a2, (int)a3);
			break;
		case LINUX_fstat:
			result = renguin_sys_fstat((int)a1, a2);
			break;
		case LINUX_newfstatat:
			result = renguin_sys_newfstatat((int)a1, a2, a3, a4);
			break;
		case LINUX_readlinkat:
			result = renguin_sys_readlinkat((int)a1, a2, a3, a4);
			break;

		// --- Memory (mem.cpp) ---
		case LINUX_brk:
			result = renguin_sys_brk(a1);
			break;
		case LINUX_mmap:
			result = renguin_sys_mmap(a1, a2, a3, a4, (int64)a5, (int64)a6);
			break;
		case LINUX_munmap:
			result = renguin_sys_munmap(a1, a2);
			break;
		case LINUX_mprotect:
			result = renguin_sys_mprotect(a1, a2, a3);
			break;

		// --- Misc (misc.cpp) ---
		case LINUX_arch_prctl:
			result = renguin_sys_arch_prctl(a1, a2);
			break;
		case LINUX_set_tid_address:
			result = renguin_sys_set_tid_address(a1);
			break;
		case LINUX_uname:
			result = renguin_sys_uname(a1);
			break;
		case LINUX_getrandom:
			result = renguin_sys_getrandom(a1, a2, a3);
			break;
		case LINUX_clock_gettime:
			result = renguin_sys_clock_gettime(a1, a2);
			break;
		case LINUX_nanosleep:
			result = renguin_sys_nanosleep(a1, a2);
			break;

		// --- Process exit (handled inline) ---
		case LINUX_exit:
			_user_exit_thread((status_t)(int)a1);
			frame->ax = 0;
			return;
		case LINUX_exit_group:
			_user_exit_team((status_t)(int)a1);
			frame->ax = 0;
			return;

		// --- Tolerated stubs so startup can proceed ---
		// Signal setup: accept and ignore until the signal stage lands.
		case LINUX_rt_sigaction:
		case LINUX_rt_sigprocmask:
		case LINUX_set_robust_list:
			result = 0;
			break;
		// No restartable-sequence support; glibc/musl fall back when told so.
		case LINUX_rseq:
			result = -LX_ENOSYS;
			break;
		// ioctl: report "not a tty" so libc's isatty()-style probes proceed.
		case LINUX_ioctl:
			result = -LX_ENOTTY;
			break;

		default:
			log_unimplemented(number);
			result = -LX_ENOSYS;
			break;
	}

	frame->ax = (uint64)result;
}
