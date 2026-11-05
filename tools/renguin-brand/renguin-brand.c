/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * renguin-brand: mark an x86-64 ELF executable as a Linux binary by setting
 * its EI_OSABI byte to ELFOSABI_LINUX (3), like FreeBSD's brandelf(1).
 * Static binaries often carry no other sign of being built for Linux.
 *
 *   renguin-brand FILE...      brand the files
 *   renguin-brand -l FILE...   only print the current OS/ABI byte
 *
 * Builds and runs on both Haiku and Linux.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EI_CLASS		4
#define EI_OSABI		7
#define ELFCLASS64		2
#define ELFOSABI_LINUX	3
#define EM_X86_64		62

static int
brand(const char* path, int listOnly)
{
	unsigned char header[20];
	int fd = open(path, listOnly ? O_RDONLY : O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}

	int result = 1;
	if (pread(fd, header, sizeof(header), 0) != (ssize_t)sizeof(header)
		|| memcmp(header, "\177ELF", 4) != 0) {
		fprintf(stderr, "%s: not an ELF file\n", path);
	} else if (header[EI_CLASS] != ELFCLASS64
		|| (header[18] | (header[19] << 8)) != EM_X86_64) {
		fprintf(stderr, "%s: not an x86-64 ELF file\n", path);
	} else if (listOnly) {
		printf("%s: OS/ABI %u%s\n", path, header[EI_OSABI],
			header[EI_OSABI] == ELFOSABI_LINUX ? " (Linux)" : "");
		result = 0;
	} else if (header[EI_OSABI] == ELFOSABI_LINUX) {
		result = 0;
	} else {
		unsigned char osabi = ELFOSABI_LINUX;
		if (pwrite(fd, &osabi, 1, EI_OSABI) != 1)
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
		else
			result = 0;
	}

	close(fd);
	return result;
}

int
main(int argc, char** argv)
{
	int listOnly = 0;
	int first = 1;
	if (argc > 1 && strcmp(argv[1], "-l") == 0) {
		listOnly = 1;
		first = 2;
	}
	if (first >= argc) {
		fprintf(stderr, "usage: %s [-l] FILE...\n", argv[0]);
		return 2;
	}

	int status = 0;
	for (int i = first; i < argc; i++)
		status |= brand(argv[i], listOnly);
	return status;
}
