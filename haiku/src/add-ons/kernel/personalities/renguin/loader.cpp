/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux x86-64 executable loader and process entry.
 *
 * Mirrors the mapping done by Haiku's elf_load_user_image() in
 * src/system/kernel/elf.cpp, but for Linux ELF binaries: PT_LOAD segments are
 * mapped at their own addresses (no self-relocation), the initial stack is
 * built in the Linux System V AMD64 format (argc, argv, envp, auxv), and the
 * process is entered with Linux register state.
 *
 * Proof-of-concept scope: statically linked ET_EXEC binaries. PT_INTERP
 * (ld-linux) and ET_DYN loading are left as the next stage; see the research
 * report, section 11.
 */


#include "renguin.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <AutoDeleter.h>

#include <arch/x86/arch_thread.h>
#include <arch/x86/64/descriptors.h>
#include <arch/x86/arch_cpu.h>
#include <arch/x86/64/iframe.h>
#include <kernel.h>
#include <team.h>
#include <thread.h>
#include <util/Random.h>
#include <vm/vm.h>
#include <vm/vm_types.h>

#include <syscalls.h>
#include <vm_defs.h>


//#define TRACE_RENGUIN_LOADER
#ifdef TRACE_RENGUIN_LOADER
#	define TRACE(x...) dprintf("renguin loader: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...) dprintf("renguin loader: " x)


static const uint64 kLinuxPageSize = 4096;


status_t
renguin_probe(const foreign_exec_args* args)
{
	if (args->header_size < sizeof(renguin_ehdr))
		return B_NOT_SUPPORTED;

	const renguin_ehdr* ehdr = (const renguin_ehdr*)args->header;
	if (memcmp(ehdr->e_ident, "\177ELF", 4) != 0)
		return B_NOT_SUPPORTED;
	if (ehdr->e_ident[RENGUIN_EI_CLASS] != RENGUIN_ELFCLASS64)
		return B_NOT_SUPPORTED;
	if (ehdr->e_machine != RENGUIN_EM_X86_64)
		return B_NOT_SUPPORTED;
	if (ehdr->e_type != RENGUIN_ET_EXEC && ehdr->e_type != RENGUIN_ET_DYN)
		return B_NOT_SUPPORTED;
	if (ehdr->e_entry == 0)
		return B_NOT_SUPPORTED;

	// Proof-of-concept: claim only binaries explicitly branded as Linux
	// (EI_OSABI == ELFOSABI_LINUX). Use the renguin-brand tool to set this
	// byte on a static binary. Detecting Linux by PT_INTERP or a GNU note is
	// the next stage (research report, section 6).
	if (ehdr->e_ident[RENGUIN_EI_OSABI] != RENGUIN_ELFOSABI_LINUX)
		return B_NOT_SUPPORTED;

	return B_OK;
}


static status_t
read_exactly(int fd, off_t pos, void* buffer, size_t size)
{
	ssize_t read = _kern_read(fd, pos, buffer, size);
	if (read < 0)
		return read;
	if ((size_t)read != size)
		return B_BAD_DATA;
	return B_OK;
}


//! Map one PT_LOAD segment, mirroring elf_load_user_image()'s rw/rx handling.
static status_t
map_segment(team_id teamID, int fd, const renguin_phdr* phdr, addr_t bias,
	int index)
{
	char name[B_OS_NAME_LENGTH];
	addr_t vaddr = phdr->p_vaddr + bias;
	addr_t pageOffset = vaddr % B_PAGE_SIZE;
	void* address = (void*)(vaddr - pageOffset);

	uint32 protection = B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA;

	size_t fileUpperBound = pageOffset + phdr->p_filesz;
	size_t memUpperBound = pageOffset + phdr->p_memsz;
	fileUpperBound = ROUNDUP(fileUpperBound, B_PAGE_SIZE);
	memUpperBound = ROUNDUP(memUpperBound, B_PAGE_SIZE);

	snprintf(name, sizeof(name), "renguin_seg%d", index);

	area_id area = vm_map_file(teamID, name, &address, B_EXACT_ADDRESS,
		fileUpperBound, protection, REGION_PRIVATE_MAP, false, fd,
		ROUNDDOWN(phdr->p_offset, B_PAGE_SIZE));
	if (area < 0) {
		ERROR("mapping segment %d failed: %s\n", index, strerror(area));
		return area;
	}

	// Zero the part of the last file page past p_filesz (partial bss).
	if (phdr->p_memsz > phdr->p_filesz) {
		addr_t zeroStart = vaddr + phdr->p_filesz;
		size_t zeroEnd = ROUNDUP(zeroStart, B_PAGE_SIZE);
		if (zeroEnd > zeroStart) {
			size_t amount = zeroEnd - zeroStart;
			if (user_memset((void*)zeroStart, 0, amount) != B_OK)
				return B_BAD_ADDRESS;
		}
	}

	// Separate anonymous area for the rest of the bss.
	if (memUpperBound > fileUpperBound) {
		void* bssAddress = (void*)((addr_t)address + fileUpperBound);
		size_t bssSize = memUpperBound - fileUpperBound;
		snprintf(name, sizeof(name), "renguin_bss%d", index);

		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address = bssAddress;
		virtualRestrictions.address_specification = B_EXACT_ADDRESS;
		physical_address_restrictions physicalRestrictions = {};
		area = create_area_etc(teamID, name, bssSize, B_NO_LOCK,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
			&virtualRestrictions, &physicalRestrictions, &bssAddress);
		if (area < 0) {
			ERROR("allocating bss %d failed: %s\n", index, strerror(area));
			return area;
		}
	}

	// Apply the real protection now that we have written the bss.
	uint32 userProtection = 0;
	if ((phdr->p_flags & RENGUIN_PF_R) != 0)
		userProtection |= B_READ_AREA;
	if ((phdr->p_flags & RENGUIN_PF_W) != 0)
		userProtection |= B_WRITE_AREA;
	if ((phdr->p_flags & RENGUIN_PF_X) != 0)
		userProtection |= B_EXECUTE_AREA;

	return vm_set_area_protection(area, userProtection, true);
}


//! Push a block of bytes onto the descending user stack, keeping sp valid.
static status_t
push_bytes(addr_t& sp, const void* data, size_t size)
{
	sp -= size;
	if (user_memcpy((void*)sp, data, size) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
push_value(addr_t& sp, uint64 value)
{
	return push_bytes(sp, &value, sizeof(value));
}


/*!	Builds the Linux initial process stack in the main thread's existing user
	stack area and returns the final (16-byte aligned) stack pointer in \a _sp.

	Layout, from high to low addresses: the argument and environment strings,
	a platform string, 16 random bytes, then the aligned auxiliary vector,
	envp array, argv array and finally argc at the lowest address, which is
	where rsp points on entry.
*/
static status_t
build_stack(Thread* thread, const foreign_exec_args* args,
	const renguin_team* data, addr_t* _sp)
{
	const uint32 argCount = args->arg_count;
	const uint32 envCount = args->env_count;
	char* const* flatArgs = args->flat_args;
	char* const* argv = flatArgs;
	char* const* envp = flatArgs + argCount + 1;

	addr_t sp = thread->user_stack_base + thread->user_stack_size;

	// Reserve room so the strings never reach the very top page.
	sp -= 16;

	// Copy strings, recording their user addresses. argCount and envCount
	// may be large, so the address arrays go on the heap, not the kernel
	// stack.
	addr_t* argvAddr = (addr_t*)malloc((argCount + 1) * sizeof(addr_t));
	addr_t* envpAddr = (addr_t*)malloc((envCount + 1) * sizeof(addr_t));
	if (argvAddr == NULL || envpAddr == NULL) {
		free(argvAddr);
		free(envpAddr);
		return B_NO_MEMORY;
	}
	MemoryDeleter argvDeleter(argvAddr);
	MemoryDeleter envpDeleter(envpAddr);

	for (uint32 i = 0; i < envCount; i++) {
		size_t len = strlen(envp[i]) + 1;
		if (push_bytes(sp, envp[i], len) != B_OK)
			return B_BAD_ADDRESS;
		envpAddr[i] = sp;
	}
	for (uint32 i = 0; i < argCount; i++) {
		size_t len = strlen(argv[i]) + 1;
		if (push_bytes(sp, argv[i], len) != B_OK)
			return B_BAD_ADDRESS;
		argvAddr[i] = sp;
	}

	// Platform string for AT_PLATFORM and the program name for AT_EXECFN.
	static const char kPlatform[] = "x86_64";
	if (push_bytes(sp, kPlatform, sizeof(kPlatform)) != B_OK)
		return B_BAD_ADDRESS;
	addr_t platformAddr = sp;

	size_t execfnLen = strlen(args->path) + 1;
	if (push_bytes(sp, args->path, execfnLen) != B_OK)
		return B_BAD_ADDRESS;
	addr_t execfnAddr = sp;

	// 16 random bytes for AT_RANDOM (used by the libc stack guard).
	uint8 random[RENGUIN_AT_RANDOM_LEN];
	for (size_t i = 0; i < sizeof(random); i++)
		random[i] = (uint8)secure_random_value();
	if (push_bytes(sp, random, sizeof(random)) != B_OK)
		return B_BAD_ADDRESS;
	addr_t randomAddr = sp;

	// The auxiliary vector. AT_SYSINFO_EHDR (the vDSO) is deliberately
	// omitted; glibc and musl fall back to real syscalls without it.
	struct { uint64 type; uint64 value; } aux[] = {
		{ RENGUIN_AT_PHDR,		data->phdr },
		{ RENGUIN_AT_PHENT,		data->phentsize },
		{ RENGUIN_AT_PHNUM,		data->phnum },
		{ RENGUIN_AT_PAGESZ,	kLinuxPageSize },
		{ RENGUIN_AT_BASE,		data->interpreter_base },
		{ RENGUIN_AT_FLAGS,		0 },
		{ RENGUIN_AT_ENTRY,		data->executable_base + data->entry },
		{ RENGUIN_AT_UID,		0 },
		{ RENGUIN_AT_EUID,		0 },
		{ RENGUIN_AT_GID,		0 },
		{ RENGUIN_AT_EGID,		0 },
		{ RENGUIN_AT_SECURE,	0 },
		{ RENGUIN_AT_CLKTCK,	1000 },
		{ RENGUIN_AT_HWCAP,		0 },
		{ RENGUIN_AT_HWCAP2,	0 },
		{ RENGUIN_AT_RANDOM,	randomAddr },
		{ RENGUIN_AT_PLATFORM,	platformAddr },
		{ RENGUIN_AT_EXECFN,	execfnAddr },
		{ RENGUIN_AT_NULL,		0 },
	};
	const int auxCount = sizeof(aux) / sizeof(aux[0]);

	// The final rsp must be 16-byte aligned. The vector below it holds:
	// argc(1) + argv(argCount) + NULL(1) + envp(envCount) + NULL(1)
	//   + auxv(auxCount * 2). Align so that rsp lands on a 16-byte boundary.
	size_t items = 1 + argCount + 1 + envCount + 1 + (size_t)auxCount * 2;
	addr_t vectorsStart = sp - items * sizeof(uint64);
	vectorsStart &= ~(addr_t)15;
	sp = vectorsStart + items * sizeof(uint64);

	// Push from the top of the vector region downward (reverse order).
	for (int i = auxCount - 1; i >= 0; i--) {
		if (push_value(sp, aux[i].value) != B_OK
			|| push_value(sp, aux[i].type) != B_OK) {
			return B_BAD_ADDRESS;
		}
	}
	if (push_value(sp, 0) != B_OK)					// envp terminator
		return B_BAD_ADDRESS;
	for (int i = envCount - 1; i >= 0; i--) {
		if (push_value(sp, envpAddr[i]) != B_OK)
			return B_BAD_ADDRESS;
	}
	if (push_value(sp, 0) != B_OK)					// argv terminator
		return B_BAD_ADDRESS;
	for (int i = argCount - 1; i >= 0; i--) {
		if (push_value(sp, argvAddr[i]) != B_OK)
			return B_BAD_ADDRESS;
	}
	if (push_value(sp, argCount) != B_OK)			// argc
		return B_BAD_ADDRESS;

	if ((sp & 15) != 0)
		ERROR("stack pointer %#lx not 16-byte aligned\n", sp);

	*_sp = sp;
	return B_OK;
}


status_t
renguin_load(foreign_exec_args* args)
{
	Team* team = args->team;
	Thread* thread = args->thread;
	int fd = args->fd;

	renguin_ehdr ehdr;
	status_t status = read_exactly(fd, 0, &ehdr, sizeof(ehdr));
	if (status != B_OK)
		return status;

	if (ehdr.e_phnum == 0 || ehdr.e_phentsize < sizeof(renguin_phdr)
		|| ehdr.e_phnum > 128) {
		ERROR("bad program header table\n");
		return B_NOT_AN_EXECUTABLE;
	}

	size_t phdrsSize = (size_t)ehdr.e_phnum * ehdr.e_phentsize;
	renguin_phdr* phdrs = (renguin_phdr*)malloc(phdrsSize);
	if (phdrs == NULL)
		return B_NO_MEMORY;
	MemoryDeleter phdrsDeleter(phdrs);

	status = read_exactly(fd, ehdr.e_phoff, phdrs, phdrsSize);
	if (status != B_OK)
		return status;

	renguin_team* data = (renguin_team*)malloc(sizeof(renguin_team));
	if (data == NULL)
		return B_NO_MEMORY;
	memset(data, 0, sizeof(*data));

	// Proof-of-concept: ET_EXEC loads at its own addresses (bias 0). ET_DYN
	// and PT_INTERP are the next stage.
	addr_t bias = 0;
	data->entry = ehdr.e_entry;
	data->phentsize = ehdr.e_phentsize;
	data->phnum = ehdr.e_phnum;
	data->brk_area = -1;
	addr_t imageTop = 0;

	bool haveInterp = false;
	addr_t firstLoadVAddr = 0;
	bool haveFirstLoad = false;

	for (uint16 i = 0; i < ehdr.e_phnum; i++) {
		const renguin_phdr* phdr
			= (const renguin_phdr*)((uint8*)phdrs + i * ehdr.e_phentsize);

		switch (phdr->p_type) {
			case RENGUIN_PT_LOAD:
			{
				status = map_segment(team->id, fd, phdr, bias, i);
				if (status != B_OK) {
					free(data);
					return status;
				}
				if (!haveFirstLoad) {
					firstLoadVAddr = phdr->p_vaddr + bias;
					haveFirstLoad = true;
				}
				addr_t segTop = phdr->p_vaddr + bias + phdr->p_memsz;
				if (segTop > imageTop)
					imageTop = segTop;
				break;
			}
			case RENGUIN_PT_INTERP:
				haveInterp = true;
				break;
			default:
				break;
		}
	}

	if (haveInterp) {
		ERROR("dynamic executables are not supported yet\n");
		free(data);
		return B_NOT_SUPPORTED;
	}

	// The program headers are mapped as part of the first PT_LOAD segment,
	// at file offset e_phoff. Point AT_PHDR there.
	data->executable_base = bias;
	data->phdr = firstLoadVAddr + ehdr.e_phoff;

	// The program break (heap) starts on the page after the last segment.
	data->brk_start = ROUNDUP(imageTop, B_PAGE_SIZE);
	data->brk_current = data->brk_start;

	status = build_stack(thread, args, data, &args->stack_pointer);
	if (status != B_OK) {
		free(data);
		return status;
	}

	args->entry = data->executable_base + data->entry;

	team->personality_data = data;
	TRACE("loaded \"%s\": entry %#lx, sp %#lx\n", args->path, args->entry,
		args->stack_pointer);
	return B_OK;
}


status_t
renguin_enter_userspace(Thread* thread, addr_t entry, addr_t stackPointer)
{
	// Linux entry state: rsp -> argc, rdx = 0 (no dynamic-linker exit hook),
	// all other general registers 0, FS/GS base 0, clean flags. The FPU state
	// is reset by the kernel when the thread first runs.
	iframe frame = {};
	frame.type = IFRAME_TYPE_SYSCALL;
	frame.ip = entry;
	frame.cs = USER_CODE_SELECTOR;
	frame.flags = X86_EFLAGS_RESERVED1 | X86_EFLAGS_INTERRUPT;
	frame.sp = stackPointer;
	frame.ss = USER_DATA_SELECTOR;

	// Linux clears FS/GS base; libc sets FS via arch_prctl() during startup.
	thread->user_local_storage = 0;

	x86_initial_return_to_userland(thread, &frame);
	return B_OK;
}


void
renguin_team_released(Team* team)
{
	if (team->personality_data != NULL) {
		free(team->personality_data);
		team->personality_data = NULL;
	}
}
