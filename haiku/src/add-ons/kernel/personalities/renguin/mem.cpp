/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux memory-management system calls: brk, mmap, munmap, mprotect.
 *
 * mmap/munmap/mprotect mirror Haiku's own POSIX mmap implementation in
 * src/system/libroot/posix/sys/mman.cpp, translating Linux flag and protection
 * values to Haiku's. brk maintains a single anonymous area for the program
 * break, since Haiku has no brk syscall of its own (see research report, §10).
 */


#include "renguin.h"
#include "linux_defs.h"

#include <KernelExport.h>
#include <OS.h>

#include <team.h>
#include <thread.h>
#include <vm/vm.h>

#include <syscalls.h>
#include <vm_defs.h>


static uint32
protection_to_haiku(uint64 prot)
{
	uint32 result = 0;
	if ((prot & LX_PROT_READ) != 0)
		result |= B_READ_AREA;
	if ((prot & LX_PROT_WRITE) != 0)
		result |= B_WRITE_AREA;
	if ((prot & LX_PROT_EXEC) != 0)
		result |= B_EXECUTE_AREA;
	return result;
}


int64
renguin_sys_brk(uint64 addr)
{
	renguin_team* data = renguin_current_team();
	if (data == NULL)
		return -LX_ENOMEM;

	// Linux brk() returns the current break on failure or a query (addr == 0),
	// never an errno.
	if (addr == 0 || addr < data->brk_start)
		return data->brk_current;

	addr_t newBreak = ROUNDUP(addr, B_PAGE_SIZE);
	size_t newSize = newBreak - data->brk_start;

	if (newSize == 0) {
		// Shrink to empty: drop the area if we have one.
		if (data->brk_area >= 0) {
			_user_unmap_memory((void*)data->brk_start,
				data->brk_current - data->brk_start);
			data->brk_area = -1;
		}
		data->brk_current = data->brk_start;
		return data->brk_current;
	}

	if (data->brk_area < 0) {
		// First growth: create the heap area at a fixed address.
		void* address = (void*)data->brk_start;
		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address = address;
		virtualRestrictions.address_specification = B_EXACT_ADDRESS;
		physical_address_restrictions physicalRestrictions = {};
		area_id area = create_area_etc(thread_get_current_thread()->team->id,
			"renguin_heap", newSize, B_NO_LOCK,
			B_READ_AREA | B_WRITE_AREA, 0, 0,
			&virtualRestrictions, &physicalRestrictions, &address);
		if (area < 0)
			return data->brk_current;
		data->brk_area = area;
	} else {
		status_t status = _user_resize_area(data->brk_area, newSize);
		if (status != B_OK)
			return data->brk_current;
	}

	data->brk_current = newBreak;
	return data->brk_current;
}


int64
renguin_sys_mmap(uint64 addr, uint64 length, uint64 prot, uint64 flags,
	int64 fd, int64 offset)
{
	if (length == 0 || (offset % B_PAGE_SIZE) != 0)
		return -LX_EINVAL;

	// Exactly one of MAP_SHARED / MAP_PRIVATE must be set.
	bool shared = (flags & LX_MAP_SHARED) != 0;
	bool priv = (flags & LX_MAP_PRIVATE) != 0;
	if (shared == priv)
		return -LX_EINVAL;

	if ((flags & LX_MAP_ANONYMOUS) != 0)
		fd = -1;
	else if (fd < 0)
		return -LX_EBADF;

	uint32 mapping = shared ? REGION_NO_PRIVATE_MAP : REGION_PRIVATE_MAP;

	uint32 addressSpec;
	void* address = (void*)addr;
	if ((flags & LX_MAP_FIXED) != 0)
		addressSpec = B_EXACT_ADDRESS;
	else if (addr != 0)
		addressSpec = B_BASE_ADDRESS;
	else
		addressSpec = B_RANDOMIZED_ANY_ADDRESS;

	uint32 areaProtection = protection_to_haiku(prot);
	if ((flags & LX_MAP_NORESERVE) != 0)
		areaProtection |= B_OVERCOMMITTING_AREA;

	// unmapAddressRange = true so MAP_FIXED replaces existing mappings, as
	// Linux requires.
	area_id area = _user_map_file("renguin mmap area", &address, addressSpec,
		(size_t)length, areaProtection, mapping, true, (int)fd, (off_t)offset);
	if (area < 0)
		return -(int64)renguin_errno_from_haiku(area);

	return (int64)(addr_t)address;
}


int64
renguin_sys_munmap(uint64 addr, uint64 length)
{
	if (length == 0 || (addr % B_PAGE_SIZE) != 0)
		return -LX_EINVAL;

	return renguin_encode(_user_unmap_memory((void*)addr, (size_t)length));
}


int64
renguin_sys_mprotect(uint64 addr, uint64 length, uint64 prot)
{
	if ((addr % B_PAGE_SIZE) != 0)
		return -LX_EINVAL;

	return renguin_encode(_user_set_memory_protection((void*)addr,
		(size_t)length, protection_to_haiku(prot)));
}
