/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux x86-64 system call dispatch.
 *
 * The kernel's system call entry (see the foreign-ABI branch in
 * src/system/kernel/arch/x86/64/interrupts.S) calls renguin_syscall() with the
 * thread's iframe whenever a thread of a Linux team makes a system call. The
 * call number is in orig_rax and the arguments are in the Linux registers
 * (rdi, rsi, rdx, r10, r8, r9). The result, or a negative Linux errno, goes
 * back in rax.
 *
 * Proof-of-concept scope: write(), exit() and exit_group(). Every other call
 * returns -ENOSYS and is logged once. The prioritized matrix in the research
 * report (section 12) is the implementation order from here.
 */


#include "renguin.h"

#include <KernelExport.h>

#include <arch/x86/64/iframe.h>
#include <team.h>
#include <thread.h>
#include <vfs.h>

#include <syscalls.h>

#include "abi/linux_syscall_names.h"


//#define TRACE_RENGUIN_SYSCALL
#ifdef TRACE_RENGUIN_SYSCALL
#	define TRACE(x...) dprintf("renguin syscall: " x)
#else
#	define TRACE(x...) ;
#endif


// Linux x86-64 system call numbers used here.
#define LINUX_SYS_write			1
#define LINUX_SYS_exit			60
#define LINUX_SYS_exit_group	231


//! Encode a Haiku result: a byte count stays, an error becomes -errno.
static int64
encode_result(ssize_t result)
{
	if (result < 0)
		return -(int64)renguin_errno_from_haiku(result);
	return result;
}


static int64
sys_write(int fd, const void* buffer, size_t count)
{
	// Linux write() has no position argument; -1 means the descriptor's
	// current offset. The buffer is a user pointer in this team's address
	// space, which _user_write() validates.
	return encode_result(_user_write(fd, -1, buffer, count));
}


static void
log_unimplemented(uint64 number)
{
	const char* name = number < LINUX_SYSCALL_COUNT
		? kLinuxSyscallNames[number] : NULL;
	Thread* thread = thread_get_current_thread();

	// Log each distinct call once per team to avoid flooding the syslog.
	dprintf("renguin: team %" B_PRId32 ": unimplemented Linux syscall %"
		B_PRIu64 " (%s)\n", thread->team->id, number,
		name != NULL ? name : "?");
}


void
renguin_syscall(struct iframe* frame)
{
	uint64 number = frame->orig_rax;

	TRACE("team %" B_PRId32 " call %" B_PRIu64 "\n",
		thread_get_current_thread()->team->id, number);

	switch (number) {
		case LINUX_SYS_write:
			frame->ax = (uint64)sys_write((int)frame->di,
				(const void*)frame->si, (size_t)frame->dx);
			return;

		case LINUX_SYS_exit:
			// Terminate just the calling thread. For the single-threaded
			// proof of concept this also ends the team.
			_user_exit_thread((status_t)(int)frame->di);
			frame->ax = 0;
			return;

		case LINUX_SYS_exit_group:
			// Terminate the whole team. _user_exit_team() posts a SIGKILL to
			// this thread, which takes the team down on the way back to user
			// space, so this does not return there.
			_user_exit_team((status_t)(int)frame->di);
			frame->ax = 0;
			return;

		default:
			log_unimplemented(number);
			frame->ax = (uint64)(-(int64)RENGUIN_ENOSYS);
			return;
	}
}
