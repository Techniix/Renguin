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

**Stage 4 code-complete for single-threaded programs; Stage 5 (dynamic
linking) loader in place. Waiting on the first boot on Haiku.**

| Done | Waiting for first boot | Next |
| --- | --- | --- |
| Process creation: `fork`/`vfork`/process-style `clone`, `execve` (with `#!` scripts), `wait4` | Alpine's busybox shell running commands | |
| Source-level research vs. FreeBSD + Haiku ([report](docs/research-report.md)) | Booting a patched Haiku and running the 12 tests ([guide](docs/TESTING.md)) | Threads (`CLONE_THREAD`), futexes |
| Kernel ABI-personality hooks (patch, applies to current `master`) | Real static glibc programs (`printf`, `malloc`, stdio) | Linux signal frames |
| Linux ELF loader: static, static-PIE and dynamic (`ld-linux`/`ld-musl`), automatic Linux detection | Alpine's dynamically linked busybox from a Linux root | `poll`/`epoll`, sockets, `/proc` |
| ~90 syscalls: memory, TLS, files and directories, descriptors, pipes, terminal `ioctl`s, ids, clocks, limits, `kill`/`tgkill` | | |
| `/compat/linux`-style Linux root with FreeBSD-style path lookup | | |
| Checked user-memory access; `load_image()` integration; `umask` | | |

**Honesty about verification:** every module source, the kernel files the
patch changes, and the patched syscall-entry assembly are **compiled against
current Haiku `master` headers in kernel mode** (`tools/check-module.sh`), and
every syscall number is checked against FreeBSD's table. The Linux test
programs are **built and verified on real Linux** to pin down the expected
behavior. Renguin has **not yet been linked with Haiku's cross-toolchain or
booted**; the environment it is developed in cannot reach Haiku's package
servers. [docs/TESTING.md](docs/TESTING.md) is the step-by-step guide for that
first boot. Claims are marked **[V]** verified / **[H]** hypothesis throughout
the research report.

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

A few small, ABI-neutral changes to Haiku's own kernel make this possible; the
rest lives in the module:

1. An **exec-time probe hook** so the kernel asks personalities before loading
   `runtime_loader` (`team.cpp`).
2. A **per-thread syscall branch** in the x86-64 entry path that preserves the
   vector registers Linux expects (`interrupts.S`).
3. A **Linux TLS path** so the kernel doesn't overwrite glibc's thread block
   (`thread.cpp`, `arch_thread.cpp`).
4. A **fork hook** so a forked Linux child keeps the Linux personality, its
   thread pointer, and undeferred signals (`team.cpp`, `thread.cpp`).
Another will be needed later: a wider signal range for glibc's internal
signals.

The `/compat/linux`-style Linux root is handled inside the module, the way
older FreeBSD releases did it per system call (`linux_emul_convpath()`), so it
needs no change to Haiku's VFS.

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
                             The Renguin kernel module:
  loader.cpp                   ELF detection and loading, Linux stack, entry
  syscall.cpp                  Linux syscall dispatch
  file.cpp tty.cpp             files, directories, descriptors, terminal ioctls
  mem.cpp                      brk, mmap and friends
  misc.cpp process.cpp         time, TLS, ids, signals, limits
  path.cpp                     the Linux root ("/compat/linux") lookup
  usermem.cpp                  checked user-memory access, scratch space
  abi/                       Linux constants/tables generated from FreeBSD (BSD-2)
tools/
  haiku-test-install.sh      Builds and installs Renguin on a Haiku machine
  test-install-script.sh     Tests that script on Linux with fake Haiku tools
  check-module.sh            Compiles everything against a Haiku checkout
  check-syscall-numbers.py   Checks syscall numbers against FreeBSD's table
  renguin-brand/             Marks a static ELF as Linux (EI_OSABI = 3)
  gen-abi.py                 Regenerates the ABI headers from a FreeBSD checkout
tests/linux/poc/             Linux test programs (raw-syscall and glibc) +
                             expected results
```

## Quick start

See **[docs/TESTING.md](docs/TESTING.md)** for the full build and test guide.
On a Haiku x86-64 machine, one script builds and installs everything:

```sh
~/renguin-build/renguin/tools/haiku-test-install.sh    # then reboot
cd ~/Desktop/renguin-tests && ./run-tests.sh .
```

By hand, in short:

```sh
# 1. Check the module against your Haiku checkout (seconds, on Linux).
tools/check-module.sh /path/to/haiku

# 2. Patch Haiku, add the module, and include it in the image.
cd /path/to/haiku
git apply /path/to/renguin/patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch
cp -r /path/to/renguin/haiku/src/add-ons/kernel/personalities src/add-ons/kernel/
echo 'AddFilesToHaikuImage system non-packaged add-ons kernel personalities : renguin ;' \
    >> build/jam/UserBuildConfig
jam -q @nightly-vmware      # from a configured generated.x86_64 directory

# 3. In the booted image: unmodified Linux programs just run.
./libc hello                 # a static glibc program
./run-tests.sh .             # the whole suite
```

## Roadmap

Development is staged so each step is testable on its own (see the report,
section 13):

1. ✅ Research and FreeBSD↔Haiku mapping
2. ✅ Linux ELF recognition and ABI selection
3. ✅ Minimal executable: `write` + `exit`
4. 🟨 Core syscalls: memory, TLS, files, descriptors, tty, ids, time and
   process creation done; signal delivery next
5. 🟨 Dynamic linking: interpreter loading and the Linux root done; needs
   testing with Alpine (musl) and Debian (glibc) userlands
6. ⬜ Threads, futexes, epoll, `/proc`
7. ⬜ Shells and command-line applications
8. ⬜ GUI: GTK, Qt, SDL, D-Bus. Plan: Linux apps speak Wayland over a Unix
   socket to a native Haiku Wayland server that turns it into Haiku windows,
   building on X512's Haiku Wayland work and its socket transport. Needs
   Unix sockets with `SCM_RIGHTS`, `memfd_create`/shared memory, `epoll`,
   `eventfd`, threads and futexes. GPU acceleration comes later through a
   Linux DRM driver (see the LinuxKPI sister project).

Alongside these: **Renguin Manager**, a native Haiku app to install a Linux
userland, show compatibility logs, and integrate with Tracker
([plan](docs/manager-app.md)). Early parts can start before stage 5 is done.

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
