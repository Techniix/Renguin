#!/usr/bin/env python3
"""Checks the LINUX_<name> = <number> enum in the Renguin dispatcher against
abi/linux_syscall_names.h, which is generated from FreeBSD's
sys/amd64/linux/syscalls.master. FreeBSD names a few calls differently
(newfstat for fstat, mmap2 for mmap, ...); those are listed in ALIASES."""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
mod = root / "haiku/src/add-ons/kernel/personalities/renguin"
names = {}
for m in re.finditer(r'/\* (\d+) \*/ "([^"]+)"',
		(mod / "abi/linux_syscall_names.h").read_text()):
	names[int(m.group(1))] = m.group(2)

ALIASES = {
	"stat": "newstat", "fstat": "newfstat", "lstat": "newlstat",
	"mmap": "mmap2", "pread64": "pread", "pwrite64": "pwrite",
	"uname": "newuname", "getrlimit": "getrlimit", "rename": "rename",
	"fstatfs": "fstatfs", "time": "time", "tkill": "tkill",
	"getdents64": "getdents64",
}

bad = 0
src = (mod / "syscall.cpp").read_text()
for name, num in re.findall(r'LINUX_(\w+)\s*=\s*(\d+)', src):
	num = int(num)
	want = {name, ALIASES.get(name, name), "linux_" + name}
	got = names.get(num)
	if got not in want and got.removeprefix("linux_") not in want:
		print(f"MISMATCH {name} = {num}: FreeBSD table says {got!r}")
		bad += 1
print(f"{len(re.findall(r'LINUX_(\w+)\s*=', src))} numbers checked, {bad} mismatches")
sys.exit(1 if bad else 0)
