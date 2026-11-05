/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGUIN_H
#define _RENGUIN_H


#include <SupportDefs.h>

#include <foreign_personality.h>


// Linux ELF constants (ELF64, x86-64).
#define RENGUIN_EI_CLASS		4
#define RENGUIN_EI_OSABI		7
#define RENGUIN_ELFCLASS64		2
#define RENGUIN_ELFOSABI_SYSV	0
#define RENGUIN_ELFOSABI_LINUX	3
#define RENGUIN_ELFOSABI_GNU	3
#define RENGUIN_ET_EXEC			2
#define RENGUIN_ET_DYN			3
#define RENGUIN_EM_X86_64		62

#define RENGUIN_PT_LOAD			1
#define RENGUIN_PT_INTERP		3
#define RENGUIN_PT_NOTE			4

#define RENGUIN_PF_X			0x1
#define RENGUIN_PF_W			0x2
#define RENGUIN_PF_R			0x4

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


// Per-team data owned by the personality, hung off Team::personality_data.
struct renguin_team {
	addr_t	executable_base;	// load bias of the main executable
	addr_t	interpreter_base;	// load bias of the ELF interpreter, or 0
	addr_t	entry;				// ELF interpreter entry (or executable entry)
	addr_t	phdr;				// address of the executable's program headers
	uint16	phentsize;
	uint16	phnum;
	addr_t	brk_start;			// reserved for a later brk() implementation
	addr_t	brk_current;
};


#ifdef __cplusplus
extern "C" {
#endif

// errno.cpp
int		renguin_errno_from_haiku(status_t status);

// loader.cpp
status_t	renguin_probe(const struct foreign_exec_args* args);
status_t	renguin_load(struct foreign_exec_args* args);
status_t	renguin_enter_userspace(Thread* thread, addr_t entry,
				addr_t stackPointer);
void		renguin_team_released(Team* team);

// syscall.cpp
void		renguin_syscall(struct iframe* frame);

#ifdef __cplusplus
}
#endif


#endif	// _RENGUIN_H
