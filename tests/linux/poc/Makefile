# Builds the proof-of-concept test programs as static, libc-free Linux
# x86-64 executables. Run this on Linux (or with a Linux cross toolchain).
CC ?= gcc
CFLAGS = -O2 -ffreestanding -fno-stack-protector -fno-pie -nostdlib -static -no-pie -Wl,--build-id=none
PROGRAMS = hello ebadf enosys exit xmm args

all: $(PROGRAMS)

%: %.S
	$(CC) $(CFLAGS) -o $@ $<

args: args.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGRAMS)

.PHONY: all clean
