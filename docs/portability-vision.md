# Renguin + Ports: a cross-platform portability vision

**Status: complementary long-term direction, not part of the current
implementation plan.** This document explores how the Linux-ABI work in
[the research report](research-report.md) could share infrastructure with a
pkgsrc-inspired software-portability framework, so the two grow as one system
rather than two disconnected projects. Nothing here changes the staged plan for
the Renguin runtime; it describes a *second track* that can start later and lean
on the first.

Facts about external projects below were checked against source: FreeBSD
`c502df9`, Haiku `dd55696`, pkgsrc `70d9df3e` (2026-11-05), and HaikuPorter
`master` (2026-11-05). Claims are marked **[V]** verified in source, **[H]**
hypothesis/design, **[U]** unverified.

## The two capabilities, and why they belong together

1. **Linux ABI compatibility (Renguin).** Run unmodified Linux x86-64 binaries
   on Haiku by translating their system calls — the Linuxulator approach.
2. **Cross-platform software portability (a ports track).** Make it easy to
   build, package, and maintain software from the Unix ecosystem *as native
   Haiku software*, handling missing APIs, build-system quirks, and
   compatibility patches automatically — the pkgsrc approach.

These look opposite — one avoids building (runs foreign binaries), one builds
natively — but they solve the **same problem from two ends**: the gap between
what software expects (a Linux/Unix platform) and what Haiku provides. A Linux
program that calls `inotify_init1` at runtime and a source package that calls
`inotify_init1` at build time are blocked by the *same missing facility*. If the
two tracks share a description of that gap, every facility Renguin gains makes
more packages buildable, and every package ported natively shrinks the set of
binaries that must run under Renguin.

The decision for any given piece of Linux software then becomes a spectrum, not
a fork in the road:

```
   run as-is          run Linux binary       build native against     build fully
   (native Haiku  <—  under Renguin      <—  a shim/compat libc   <—  native Haiku
    .hpkg exists)      (/compat/linux)        (partial port)           (clean port)
       best for             fallback for          middle ground          best outcome
     already-ported      closed-source apps      ABI-heavy software     for the ecosystem
```

A single front-end should place each package on that spectrum automatically,
using one shared body of knowledge.

## What already exists (so we don't rebuild it)

**pkgsrc already targets Haiku. [V]** `mk/platform/Haiku.mk` defines Haiku's
paths and capabilities (`_OPSYS_LIB_DIRS=/boot/system/develop/lib …`,
`_OPSYS_SHLIB_TYPE=ELF`, `_OPSYS_PTHREAD_AUTO=yes`, symlink-only wrappers because
Haiku lacked hardlinks), and `bootstrap/README.Haiku` documents bootstrapping on
R1/beta5 with GCC 13. So "pkgsrc on Haiku, building native Haiku software" is not
hypothetical — it works today. The vision is not to create a ports framework but
to **connect pkgsrc's existing machinery to the Renguin ABI and to Haiku's own
package kit.**

**Haiku already has a native ports system. [V]** HaikuPorter builds `.recipe`
files (bash with `SUMMARY`, `SOURCE_URI`, `CHECKSUM_SHA256`, `PATCHES`,
`ARCHITECTURES`, `PROVIDES`/`REQUIRES`/`BUILD_REQUIRES`, and `BUILD()`/
`INSTALL()`/`TEST()` functions) into `.hpkg` packages, resolving dependencies
through the Haiku package kit and activating them through `packagefs`. Patches
live in per-port `.patchset` files. This is Haiku's equivalent of a ports tree
and the primary way software reaches users.

So three systems already exist and the work is **integration**, not greenfield:
Renguin (runtime, this repo), pkgsrc (cross-platform build), HaikuPorter (native
Haiku packaging).

## The three pkgsrc mechanisms worth borrowing

pkgsrc's portability comes from four design ideas, each of which has a direct
Renguin analog. This is the heart of the shared-infrastructure argument.

| pkgsrc mechanism | What it does [V] | Renguin analog | Shared opportunity |
| --- | --- | --- | --- |
| `mk/platform/<OPSYS>.mk` — one file per OS | Abstracts each OS's paths, tools, and quirks behind variables; `uname -s` selects it | The personality module — one per ABI (`foreign_personality.h`) | Both are **target definitions**. A single "target descriptor" format could describe an OS for *both* building and running. |
| `CROSS_DESTDIR` cross-build sysroot | Builds into a sysroot checked for completeness (`$CROSS_DESTDIR/usr/include/stddef.h`); `TARGET_OPSYS`/`TARGET_MACHINE_PLATFORM` name the target | Renguin's `/compat/linux` runtime root (report §11) | **Same directory.** Build Linux software into `/compat/linux`, then run it there under Renguin. One sysroot, built and executed. |
| `buildlink3` dependency sandbox | Symlink farms expose *only declared* dependencies to a build and rewrite `-I`/`-L` (`find-headers.mk`, `find-libs.mk`) | The per-team overlay-root lookup (report §10) | Both control *which* headers/libs/files a process sees. The overlay-root resolver and buildlink are the same idea at runtime vs build time. |
| `patches/` + `.patchset` compatibility patches | Per-package patches that fix OS-specific build breaks | Renguin's syscall/ioctl gap list (report §12 matrix) | A **shared compatibility knowledge base** (below): a missing API at build time and a missing syscall at runtime are one catalog. |

## The shared infrastructure: a Compatibility Knowledge Base

The single most valuable piece of shared infrastructure is a machine-readable
**Compatibility Knowledge Base (CKB)** that both tracks consume and feed. **[H]**

It records, per facility (a syscall, an ioctl, a libc API, an ABI struct, a
`/proc` file, a build-system assumption):

- **Runtime status** from Renguin: implemented / stubbed / missing, with the
  syscall matrix (report §12) as the seed. Renguin's `log_unimplemented()`
  already emits exactly this signal — "team N used unimplemented Linux syscall
  K" — which can be aggregated into the CKB automatically.
- **Build status** from the ports track: does Haiku's native libroot provide
  this API? Is there a shim? A known patch?
- **Provenance and patches**: links to the FreeBSD handler, the Haiku function,
  and any compatibility patch that bridges the gap.

Three consumers read it:

1. **Renguin** (runtime) — already the source of the "missing syscall" signal;
   could also read the CKB to pick better stub behavior.
2. **The native ports track** — decides whether a package's dependencies are
   satisfiable natively, and pulls in the right shims and patches.
3. **A decision front-end** (`renguin install <pkg>`) — places each package on
   the spectrum above: native `.hpkg` if one exists; native build if the CKB
   says its APIs are covered; Linux binary under Renguin otherwise.

This closes a feedback loop that neither FreeBSD's Linuxulator nor pkgsrc has on
its own: **running Linux binaries generates the exact prioritization data the
porting effort needs.** The syscalls real apps actually hit float to the top of
Renguin's backlog, and the libraries real builds actually need float to the top
of the porting backlog.

## Opportunities for code and design reuse

- **One sysroot tree.** `/compat/linux` serves as Renguin's runtime root *and*
  pkgsrc's `CROSS_DESTDIR` when building for the Linux-on-Haiku target. Build and
  run share a filesystem; no duplication. **[H]**
- **pkgsrc wholesale, as a cross-target.** Building Linux binaries that run under
  Renguin is, in pkgsrc terms, a cross-build to `TARGET_OPSYS=Linux` with
  `CROSS_DESTDIR=/compat/linux`. pkgsrc's existing cross machinery, tool
  wrappers, and 20,000+ packages apply with a new target descriptor rather than
  new framework code. **[H, U on completeness]**
- **HaikuPorter recipes as the native front.** Software that ports cleanly stays
  a normal `.recipe`/`.hpkg`; the CKB just informs which those are. No new
  packaging format for the common case. **[V on HaikuPorter; H on integration]**
- **The overlay-root resolver.** The `vfs.cpp` alternate-root lookup Renguin
  needs (report §10) is the same mechanism a buildlink-style sandbox wants; build
  one, use it for both. **[H]**
- **The gap catalog seeds itself.** The syscall matrix (report §12) and
  `log_unimplemented()` output are the CKB's first rows at no extra cost. **[V]**

## Existing projects worth studying

| Project | Direction | Lesson for us |
| --- | --- | --- |
| **FreeBSD Linuxulator + `linux_base`/`debootstrap`** | Linux ABI on FreeBSD, with a packaged Linux userland | The reference for both the ABI layer and the "install a Linux root from real distro packages" story. Their CentOS/Debian userlands show the `/compat/linux` population path. **[V]** |
| **pkgsrc** | Portable native building across 20+ OSes | The target-descriptor + sysroot + buildlink + patch model we borrow. Already on Haiku. **[V]** |
| **HaikuPorts / HaikuPorter** | Native Haiku packaging | The native front-end and `.hpkg`/packagefs integration we must not bypass. **[V]** |
| **WSL 1** | Linux syscall translation on the NT kernel (no VM) | Closest architectural sibling to Renguin: a `pico-process` personality translating Linux syscalls. Its eventual pivot to a real kernel (WSL 2) is a cautionary data point about where translation stops being worth it (deep `/proc`, namespaces, iptables). **[U — from public design writeups]** |
| **Darling** | macOS binaries on Linux | Mach-O loader + Darwin syscall translation + a from-packages userland — the mirror image of Renguin, with a mature "foreign userland tree" design. **[U]** |
| **Gentoo Prefix / Nixpkgs** | Relocatable native toolchains in a prefix | Models for a self-contained prefix that doesn't fight the host OS — relevant to keeping the Linux world out of Haiku's native `/boot`. **[U]** |
| **box64** | Userspace x86 emulation with native lib thunking | A different reuse model: thunk hot libraries to native ones. Interesting long-term for mixing native Haiku libs into Linux processes, but out of scope early. **[U]** |

## Challenges and how they bound the design

- **pkgsrc expects a Unix-shaped host. [V]** `Haiku.mk` already carries Haiku
  workarounds (symlink-only wrappers, no hardlinks, `can't use readelf`), and the
  default `/usr/pkg` prefix doesn't fit Haiku's read-only system root — bootstrap
  uses `/boot/pkg`. The Linux-target path adds a second axis (host Haiku, target
  Linux) that pkgsrc supports in principle but that is lightly tested for this
  pair. Expect real bring-up work. **[H]**
- **Two package databases.** pkgsrc has its own DB; Haiku has packagefs/`.hpkg`.
  The native track should emit `.hpkg` (HaikuPorter already does), so the CKB and
  front-end — not a parallel package manager — are where integration lives.
  Avoid a third, competing package world. **[H]**
- **Keeping Linux out of native Haiku. [H]** The firmest rule: Linux headers,
  libs, and the `/compat/linux` tree must never leak into native Haiku builds or
  the native `/boot`. The sysroot/overlay isolation is what enforces this; it is
  a feature, not an accident, and it must be designed in from the first native
  build target. This is also what preserves Haiku's native UX — a Haiku user who
  never installs a Linux app sees none of this.
- **Licensing stays clean.** The CKB and descriptors are new MIT work; borrowing
  pkgsrc *concepts* carries no license burden, and pkgsrc's own BSD-licensed `mk`
  files would only be used unmodified if we ship a pkgsrc tree, which is a
  packaging choice, not a code-copy. The report's licensing rules (avoid BSD-4
  files) continue to apply to any ABI tables shared with the CKB. **[V on
  licenses]**
- **Scope discipline.** This track must not slow the Renguin runtime. It has no
  dependency on the runtime past Stage 4 and should not start in earnest until
  Renguin can run dynamically linked glibc/musl programs (report Stage 5), at
  which point a `/compat/linux` tree exists to build into.

## A staged roadmap (parallel to, not ahead of, the Renguin plan)

Each stage is independently useful and gated on a Renguin milestone, so the
runtime always leads.

- **P0 — Seed the CKB (can start now).** Emit `log_unimplemented()` output into a
  structured log; publish the syscall matrix (report §12) as the CKB's first
  machine-readable rows. Cost: tiny. Value: real apps start voting on the backlog.
  *Gate: none.*
- **P1 — Populate `/compat/linux` from real distro packages.** A tool that pulls
  a musl/Alpine or glibc/Debian root into `/compat/linux` (FreeBSD's
  `debootstrap` model), reusing the overlay-root resolver. *Gate: Renguin Stage 5
  (dynamic linking).*
- **P2 — pkgsrc Linux-on-Haiku cross target.** A target descriptor pointing
  pkgsrc's `CROSS_DESTDIR` at `/compat/linux`, `TARGET_OPSYS=Linux`; prove it by
  cross-building one leaf package and running it under Renguin. *Gate: P1.*
- **P3 — The `renguin install` front-end.** One command that reads the CKB and
  the HaikuPorts catalog and chooses native `.hpkg` / native build / Linux binary
  per package, with the choice explained. *Gate: P2 + a HaikuPorts query path.*
- **P4 — Native-build shims.** For packages close to native-portable, provide
  small Haiku shims for the handful of APIs the CKB flags, turning "run as Linux
  binary" into "native Haiku `.hpkg`" one library at a time, worst-offenders
  first (driven by P0 data). *Gate: ongoing.*

The end state: a Haiku user types one command, and the system quietly picks the
best way to get that software running — a native package if the ecosystem has
one, a freshly built native package if it can, or a Linux binary under Renguin if
it must — while Haiku's own applications and system stay exactly as they are.

## Relationship to the current plan

Nothing above reprioritizes the Renguin runtime. The runtime roadmap (report
§13) is unchanged; this track is strictly downstream of it and begins no earlier
than Stage 5. The only thing worth doing *now* is P0 — structured
`log_unimplemented()` output — because it is nearly free and starts collecting
the data that will make every later stage of *both* tracks cheaper.
