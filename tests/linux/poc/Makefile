# Builds the Renguin test programs as static Linux x86-64 executables.
# Run this on Linux (or with a Linux cross toolchain).
#
# The raw-syscall programs need no libc. "libc" and "libc-pie" need a static
# C library: glibc's libc.a (Debian/Ubuntu: libc6-dev) or musl (CC=musl-gcc).
CC ?= gcc
RAWFLAGS = -O2 -ffreestanding -fno-stack-protector -fno-pie -nostdlib \
	-static -no-pie -Wl,--build-id=none
RAW = hello ebadf enosys exit xmm args mem fileio ids proc
LIBC = libc libc-pie
PROGRAMS = $(RAW) $(LIBC)

all: $(PROGRAMS) branded

# The libc-free programs carry no Linux marker (EI_OSABI 0, no ABI note), so
# Renguin would not claim them. Mark them, like FreeBSD's brandelf. Linux
# ignores the byte, so they still run there. The glibc programs are already
# branded GNU/Linux by the toolchain.
BRAND = ../../../tools/renguin-brand/renguin-brand.c
renguin-brand: $(BRAND)
	$(CC) -O2 -o $@ $<

branded: $(RAW) renguin-brand
	for p in $(RAW); do ./renguin-brand $$p || exit 1; done
	touch $@

%: %.S
	$(CC) $(RAWFLAGS) -o $@ $<

args mem: %: %.c
	$(CC) $(RAWFLAGS) -o $@ $<

fileio ids proc: %: %.c sys.h
	$(CC) $(RAWFLAGS) -o $@ $<

libc: libc.c
	$(CC) -O2 -static -o $@ $<

# Static PIE: an ET_DYN executable with no interpreter, loaded at a random
# base. Tests Renguin's ET_DYN path without needing a Linux userland.
libc-pie: libc.c
	$(CC) -O2 -static-pie -fPIE -o $@ $<

clean:
	rm -f $(PROGRAMS) renguin-brand branded

.PHONY: all clean
