/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux miscellaneous system calls: arch_prctl, set_tid_address, uname,
 * getrandom, clock_gettime, nanosleep.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <arch/x86/arch_thread.h>
#include <thread.h>
#include <util/Random.h>


//! arch_prctl: set or read the FS/GS base. ARCH_SET_FS is how glibc and musl
//! install thread-local storage on x86-64. Setting user_local_storage and
//! calling x86_set_tls_context() applies the FS base immediately and keeps it
//! across context switches (see the foreign-ABI case in arch_thread.cpp).
int64
renguin_sys_arch_prctl(uint64 code, uint64 addr)
{
	Thread* thread = thread_get_current_thread();

	switch (code) {
		case LX_ARCH_SET_FS:
			thread->user_local_storage = (addr_t)addr;
			x86_set_tls_context(thread);
			return 0;

		case LX_ARCH_SET_GS:
			thread->arch_info.user_gs_base = (uint64)addr;
			x86_set_tls_context(thread);
			return 0;

		case LX_ARCH_GET_FS:
		{
			uint64 value = thread->user_local_storage;
			if (user_memcpy((void*)addr, &value, sizeof(value)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}

		case LX_ARCH_GET_GS:
		{
			uint64 value = thread->arch_info.user_gs_base;
			if (user_memcpy((void*)addr, &value, sizeof(value)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}

		default:
			return -LX_EINVAL;
	}
}


//! set_tid_address: records where the kernel should clear the thread id and
//! wake a futex on thread exit (used by pthread_join). Returns the caller's
//! thread id. The clear-on-exit action is wired up when threads land (futex
//! stage); for now the address is just stored.
int64
renguin_sys_set_tid_address(uint64 tidptr)
{
	renguin_team* data = renguin_current_team();
	if (data != NULL)
		data->clear_child_tid = (addr_t)tidptr;
	return thread_get_current_thread()->id;
}


int64
renguin_sys_uname(uint64 buffer)
{
	linux_utsname info;
	memset(&info, 0, sizeof(info));
	strlcpy(info.sysname, "Linux", sizeof(info.sysname));
	strlcpy(info.nodename, "haiku", sizeof(info.nodename));
	// Report a modern-enough kernel; some glibc builds gate features on this.
	strlcpy(info.release, "6.1.0-renguin", sizeof(info.release));
	strlcpy(info.version, "#1 Renguin", sizeof(info.version));
	strlcpy(info.machine, "x86_64", sizeof(info.machine));

	if (user_memcpy((void*)buffer, &info, sizeof(info)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_getrandom(uint64 buffer, uint64 length, uint64 flags)
{
	(void)flags;	// GRND_NONBLOCK/GRND_RANDOM: the kernel RNG never blocks.

	// Fill in word-sized chunks from the kernel's secure RNG.
	size_t remaining = (size_t)length;
	addr_t dest = (addr_t)buffer;
	while (remaining > 0) {
		uint32 word = secure_random_value();
		size_t chunk = remaining < sizeof(word) ? remaining : sizeof(word);
		if (user_memcpy((void*)dest, &word, chunk) != B_OK)
			return -LX_EFAULT;
		dest += chunk;
		remaining -= chunk;
	}
	return (int64)length;
}


int64
renguin_sys_clock_gettime(uint64 clockId, uint64 timespecPtr)
{
	bigtime_t micros;
	switch (clockId) {
		case LX_CLOCK_REALTIME:
			micros = real_time_clock_usecs();
			break;
		case LX_CLOCK_MONOTONIC:
			micros = system_time();
			break;
		default:
			return -LX_EINVAL;
	}

	linux_timespec ts;
	ts.tv_sec = micros / 1000000;
	ts.tv_nsec = (micros % 1000000) * 1000;
	if (user_memcpy((void*)timespecPtr, &ts, sizeof(ts)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_nanosleep(uint64 request, uint64 remain)
{
	linux_timespec ts;
	if (user_memcpy(&ts, (void*)request, sizeof(ts)) != B_OK)
		return -LX_EFAULT;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
		return -LX_EINVAL;

	bigtime_t micros = (bigtime_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	bigtime_t remaining = 0;
	status_t status = _user_snooze_etc(micros, B_SYSTEM_TIMEBASE,
		B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT, &remaining);

	if (status == B_INTERRUPTED) {
		if (remain != 0) {
			linux_timespec left;
			left.tv_sec = remaining / 1000000;
			left.tv_nsec = (remaining % 1000000) * 1000;
			user_memcpy((void*)remain, &left, sizeof(left));
		}
		return -4;	// -EINTR
	}
	if (status != B_OK)
		return -(int64)renguin_errno_from_haiku(status);
	return 0;
}
