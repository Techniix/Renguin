<p align="center">
  <img src="assets/renguin-banner.png" alt="Renguin — teaching Haiku to run Linux applications" width="820">
</p>

<p align="center">
  <em>A native Linux x86-64 binary compatibility layer for Haiku, in the spirit of FreeBSD's Linuxulator.</em>
</p>

---

## What this is

Renguin teaches [Haiku](https://www.haiku-os.org/) to run **unmodified Linux
x86-64 programs** directly — no virtual machine, no QEMU, no Linux guest
kernel, no container. A Linux binary becomes just another process on Haiku,
the way Linux binaries run on FreeBSD through its
[Linuxulator](https://docs.freebsd.org/en/books/handbook/linuxemu/).

The approach is a **kernel ABI personality**: when Haiku is about to start a
program, a small kernel hook lets a loadable module claim it. Renguin claims
Linux ELF binaries, maps them, builds a Linux-format process, and from then on
every system call that process makes is translated into Haiku's own kernel
facilities. Native Haiku programs never touch any of this.

> **We are not running Linux inside Haiku. We are teaching Haiku to run Linux
> applications.**

## Project status

**Stage 4 in progress — the core syscalls a static program needs are landing.**
This is early, pre-boot work.

| Done | In progress | Next |
| --- | --- | --- |
| Source-level research vs. FreeBSD + Haiku ([report](docs/research-report.md)) | Booting & running on real Haiku | Process: `clone`/`fork`/`wait4` |
| Kernel ABI-personality hooks (patch) | Dynamic linking (`ld-linux`, glibc/musl) | Threads, futexes, signals |
| Linux ELF loader + syscall dispatch (module) | | `/compat/linux` path overlay |
| `write`, `exit`, `exit_group` (Stage 3) | | |
| Memory (`brk`, `mmap`, `munmap`, `mprotect`), TLS (`arch_prctl`), core file I/O (`openat`, `read`, `writev`, `fstat`…), `uname`, `getrandom`, `clock_gettime` | | |

**Honesty about verification:** every source file here is **type-checked and
assembled against the Haiku `master` headers** with a host compiler, and the
Linux test programs are **built and verified on real Linux** to pin down the
expected behavior. Renguin has **not yet been built with the full Haiku
cross-toolchain or booted**, because that toolchain and Haiku's package servers
aren't reachable from the environment this was developed in. The first thing a
Haiku developer should do is the full build in [docs/TESTING.md](docs/TESTING.md)
and tell us what breaks. Claims are marked **[V]** verified / **[H]** hypothesis
throughout the research report.

## How it works

```
                 Haiku kernel                          Renguin module
                 ────────────                          ──────────────
  exec a program ──► team_create_thread_start ──probe──► "is this Linux?"
                         │  (not Linux)                      │ (yes)
                         ▼                                   ▼
                   runtime_loader                    map ELF, build Linux
                   (native Haiku)                    stack, enter userland
                                                            │
  Linux `syscall` ──► x86_64_syscall_entry ──foreign──► translate to Haiku
                     (one extra branch)                _user_* / vm / vfs
```

Four small, ABI-neutral changes to Haiku's own kernel make this possible; the
rest lives in the module:

1. An **exec-time probe hook** so the kernel asks personalities before loading
   `runtime_loader` (`team.cpp`).
2. A **per-thread syscall branch** in the x86-64 entry path that preserves the
   vector registers Linux expects (`interrupts.S`).
3. A **Linux TLS path** so the kernel doesn't overwrite glibc's thread block
   (`thread.cpp`, `arch_thread.cpp`).
4. Later stages add a wider signal range and a `/compat/linux` path overlay.

The full rationale, with exact FreeBSD and Haiku source references, is in the
**[research report](docs/research-report.md)** — the heart of this repository.

For the longer-term direction — how the Linux ABI layer could share
infrastructure with a [pkgsrc](https://www.pkgsrc.org/)-inspired software
portability framework, so building and running foreign software become one
system — see **[docs/portability-vision.md](docs/portability-vision.md)**. It is a
complementary track, downstream of the runtime work, not a change to the plan
above.

## Repository layout

```
assets/                      Logo and banner (original artwork)
docs/
  research-report.md         Source-level FreeBSD↔Haiku study and the plan
  TESTING.md                 How to build the patched kernel, module, and tests
patches/haiku/               Patch adding the ABI-personality hooks to Haiku
haiku/src/add-ons/kernel/personalities/renguin/
                             The Renguin kernel module (loader, dispatch, syscalls)
  abi/                       Linux constants/tables generated from FreeBSD (BSD-2)
tools/
  renguin-brand/             Marks a static ELF as Linux (EI_OSABI = 3)
  gen-abi.py                 Regenerates the ABI headers from a FreeBSD checkout
tests/linux/poc/             Tiny libc-free Linux test programs + expected results
```

## Quick start

See **[docs/TESTING.md](docs/TESTING.md)** for the full build. In short, on a
Haiku x86-64 build host:

```sh
# 1. Apply the kernel hooks and build Haiku with the Renguin module.
cd /path/to/haiku
git apply /path/to/renguin/patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch
cp -r /path/to/renguin/haiku/src/add-ons/kernel/personalities src/add-ons/kernel/
jam -q haiku.hpkg        # or a full image

# 2. On the booted Haiku system, brand and run a static Linux binary.
renguin-brand ./hello        # set EI_OSABI = ELFOSABI_LINUX
./hello                      # runs through Renguin
```

The proof-of-concept target is a statically linked, libc-free Linux program
that prints `hello` and exits with status 42 — see `tests/linux/poc/`.

## Roadmap

Development is staged so each step is testable on its own (see the report,
section 13):

1. ✅ Research and FreeBSD↔Haiku mapping
2. ✅ Linux ELF recognition and ABI selection
3. ✅ Minimal executable: `write` + `exit`
4. 🟨 Core syscalls: memory, TLS, and file I/O done; process and signals next
5. ⬜ Dynamic linking: `ld-linux`, glibc, musl
6. ⬜ Threads, futexes, epoll, `/proc`
7. ⬜ Shells and command-line applications
8. ⬜ GUI: GTK, Qt, SDL, D-Bus

The long-term aim is for installing and running a Linux application on Haiku to
feel as natural as running a native one, eventually reaching software shipped
as Debian packages and Flatpaks.

## Licensing

Renguin's own code is under the **MIT License** (matching Haiku) — see
[LICENSE](LICENSE). Some ABI constant and table headers are **generated from
FreeBSD's Linuxulator** (`BSD-2-Clause`) and keep their original notices; see
[NOTICE](NOTICE). No GPL code is used, and the advertising-clause BSD-4 files in
FreeBSD are deliberately avoided.

## Credits and references

Renguin stands on the shoulders of FreeBSD's Linuxulator (its architectural
reference) and Haiku's kernel. It is an independent project and is not
affiliated with or endorsed by the Haiku or FreeBSD projects. The name blends
*renga* (Japanese linked verse) with *penguin*; the logo is an original ink-wash
mark — a penguin at an old pond, with the 5-7-5 strokes of a haiku.

- [FreeBSD Linuxulator source](https://github.com/freebsd/freebsd-src/tree/main/sys/compat/linux)
- [Haiku source](https://github.com/haiku/haiku)
- [Linux Test Project](https://github.com/linux-test-project/ltp)

<p align="center"><sub>An old pond, quiet — a penguin slips through the gate and swims in new water.</sub></p>
