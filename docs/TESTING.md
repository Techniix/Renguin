# Building and testing Renguin

This is pre-boot, proof-of-concept code. Nothing here has been run on a booted
Haiku system yet (see "Verification status" below). These are the steps to try
it; please report what happens.

## What you need

- A Haiku **x86-64** build environment (a checkout of
  [haiku/haiku](https://github.com/haiku/haiku) and its cross-toolchain, built
  per the [Haiku build guide](https://www.haiku-os.org/guides/building/)).
- A **Linux x86-64 toolchain** to build the test programs (any `gcc`/`clang`
  that targets Linux; `musl-gcc` is ideal). You can build the test binaries on
  any Linux machine and copy them over.

The kernel hooks were developed against Haiku `master` at commit
`dd55696fdbd4e8e512a98c2a7637b07ff14496fe` (recorded in
`patches/haiku/BASE_COMMIT`). If your checkout differs and the patch does not
apply cleanly, the hunks are small and easy to re-apply by hand.

## 1. Patch and build Haiku with the Renguin module

```sh
cd /path/to/haiku

# Apply the kernel ABI-personality hooks.
git apply /path/to/renguin/patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch

# Drop the Renguin module into the tree. Its Jamfile is picked up by the
# new "personalities" SubInclude the patch adds to the kernel add-ons Jamfile.
cp -r /path/to/renguin/haiku/src/add-ons/kernel/personalities \
      src/add-ons/kernel/

# Build. The module is x86-64 only.
jam -q "<build>package" haiku.hpkg     # or build a full bootable image
```

The module builds as `personalities/renguin/v1` under the kernel add-ons. It is
found and loaded lazily the first time any program is started after boot, via
the module prefix `personalities` (see `foreign_personality.cpp`).

> If `jam` cannot find the module, confirm the patch added
> `SubInclude HAIKU_TOP src add-ons kernel personalities ;` to
> `src/add-ons/kernel/Jamfile`, and that the `personalities` directory copied in.

## 2. Build the test programs (on Linux)

```sh
cd /path/to/renguin/tests/linux/poc
make                      # builds: hello ebadf enosys exit xmm args
./run-tests.sh .          # sanity-check them on Linux first (all should pass)
```

These are statically linked, libc-free programs that use raw syscalls only, so
they are a clean test of the ABI itself. `expected.txt` records the exit status
and stdout each one must produce.

## 3. Build the brand tool

Renguin's proof-of-concept stage only claims binaries whose ELF `EI_OSABI` byte
is `ELFOSABI_LINUX` (3). Static Linux binaries usually carry `0` (System V), so
you mark them first, like FreeBSD's `brandelf`:

```sh
# Builds on both Linux and Haiku.
cc -O2 -o renguin-brand /path/to/renguin/tools/renguin-brand/renguin-brand.c
./renguin-brand hello          # set the Linux brand
./renguin-brand -l hello       # verify: "OS/ABI 3 (Linux)"
```

## 4. Run on Haiku

Copy the branded test binaries and `run-tests.sh`/`expected.txt` to the booted
Haiku system, then:

```sh
./hello ; echo $?              # expect: hello   then   42
./run-tests.sh .               # expect: 6 passed, 0 failed
```

Watch the kernel log (`tail -f /var/log/syslog`, or the on-screen debug output)
for lines beginning `renguin:`. You should see the module load and, for any
unimplemented call, a single `unimplemented Linux syscall N (name)` line.

## What each test checks

| Program | Checks | Expected |
| --- | --- | --- |
| `hello` | `write` + `exit_group` | prints `hello`, exits 42 |
| `exit` | `exit` (single thread) | exits 7 |
| `ebadf` | errno translation | `write(99,…)` → exit 9 (`EBADF`) |
| `enosys` | unknown syscall | exits 38 (`ENOSYS`) |
| `xmm` | vector registers survive a syscall | exits 0 |
| `args` | argv/envp/auxv stack layout | exits 0, echoes args |
| `mem` | `mmap`/`munmap`/`brk`/`uname` (Stage 4) | prints `ok`, exits 0 |

The `xmm` test is the important early regression check: it fails if the kernel
clears the vector registers on the Linux syscall return path (see the research
report, section 7).

## Regenerating the ABI headers

The Linux constant and table headers under
`haiku/src/add-ons/kernel/personalities/renguin/abi/` are generated from a
FreeBSD checkout, so they can be refreshed as FreeBSD's Linuxulator evolves:

```sh
python3 tools/gen-abi.py /path/to/freebsd-src \
    haiku/src/add-ons/kernel/personalities/renguin/abi \
    $(git -C /path/to/freebsd-src rev-parse HEAD)
```

## Verification status

What has and has not been done, precisely:

- **Done:** every kernel and module source file is type-checked against the
  Haiku `master` headers with a host compiler, the modified x86-64 assembly is
  preprocessed and assembled, and all six Linux test programs are built and
  pass on real Linux (establishing the expected results).
- **Not done:** a full build with the Haiku cross-toolchain, and booting/running
  on Haiku. The development environment could not reach Haiku's package servers
  or build its cross-compiler, so this crucial step is open. Expect to fix real
  bugs here — this is where testing on actual Haiku begins.
