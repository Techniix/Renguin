# Building and testing Renguin

Renguin has not yet run on a booted Haiku system. Every file is checked
against Haiku's real headers (section 0), and the test programs pass on real
Linux, but the first boot is where real bugs will show up. These are the steps
to get there. Please report what happens (section 8).

**Short version:** apply the patch to a Haiku checkout, add one line to
`UserBuildConfig`, build `haiku.vmdk`, boot it in VirtualBox, run
`./run-tests.sh` in the test folder on the Desktop, and send back the output
and `grep renguin /var/log/syslog`.

---

## What you need

- A machine that can build Haiku x86-64. Either:
  - **Linux** (or WSL2 on Windows, inside the Linux filesystem, which is
    case-sensitive): the official cross build from Haiku's
    [`ReadMe.Compiling.md`](https://github.com/haiku/haiku/blob/master/ReadMe.Compiling.md),
    or
  - **Haiku itself** (x86-64): it ships the needed compilers.
- **VirtualBox** (or QEMU) to boot the result. Use a VM for the first kernel
  test, not your main install: a kernel patch can fail to boot.
- About 30 GB of disk space and an hour or two for the first full build.
  Later builds are incremental.

The patch was checked against Haiku `master` at
`fd8aec24d515c1a6d4873983bb8b7fa2ac592a47` (2026-11-06). It was written
against `dd55696f` (`patches/haiku/BASE_COMMIT`) and applies to both.

---

## 0. Pre-flight check (2 minutes, any Linux machine)

Before spending an hour on a build, make sure the module still compiles
against *your* Haiku checkout. This uses the host `g++` in Haiku kernel mode,
so it catches wrong API names, signatures and header changes. After you apply
the patch (step 1) it also checks the patched kernel files and assembles the
syscall-entry assembly.

```sh
/path/to/renguin/tools/check-module.sh /path/to/haiku
```

Every line should say `ok`. Also:

```sh
python3 /path/to/renguin/tools/check-syscall-numbers.py   # 0 mismatches
```

---

## 1. Patch Haiku and add the module

```sh
git clone https://github.com/haiku/haiku.git
git clone https://github.com/haiku/buildtools.git   # Linux builds only
cd haiku

# The kernel ABI-personality hooks (small, ABI-neutral).
git apply /path/to/renguin/patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch

# The Renguin module source.
cp -r /path/to/renguin/haiku/src/add-ons/kernel/personalities src/add-ons/kernel/
```

Now tell the image build to include the module, and the test programs. Create
`build/jam/UserBuildConfig` (or add to it):

```jam
# Renguin: install the module as a non-packaged kernel add-on, so later
# module-only updates are a file copy (section 7) instead of a rebuild.
AddFilesToHaikuImage system non-packaged add-ons kernel personalities
	: renguin ;

# Renguin test programs, copied to ~/Desktop/renguin-tests. Build or unpack
# them first (section 3). Arguments: target dir : source dir : new name.
CopyDirectoryToHaikuImage home Desktop
	: /path/to/renguin/tests/linux/poc : renguin-tests ;
```

(Rule signatures from `build/jam/ImageRules`:
`AddFilesToHaikuImage <dir tokens> : <targets>` and
`CopyDirectoryToHaikuImage <dir tokens> : <source dir> : <target name>`.)

Why this matters: building a kernel add-on does not put it into the image.
Haiku's image lists its kernel add-ons explicitly (`build/jam/packages/Haiku`).
Without the `AddFilesToHaikuImage` line, the image boots without Renguin.

---

## 2. Build a bootable image

**On Linux** (from `ReadMe.Compiling.md`):

```sh
mkdir generated.x86_64 && cd generated.x86_64
../configure --cross-tools-source ../../buildtools --build-cross-tools x86_64
jam -q -j$(nproc) @nightly-vmware        # produces haiku.vmdk
```

**On Haiku** (x86-64), configure for the native compilers instead. This
follows the ReadMe's pattern for Haiku hosts; check `../configure --help` if
your version differs:

```sh
mkdir generated.x86_64 && cd generated.x86_64
../configure --target-arch x86_64
jam -q -j4 @nightly-vmware
```

The build downloads packages from Haiku's servers, so it needs network
access. Use `@nightly-anyboot` instead for an ISO/USB image for real
hardware.

Before the full build, you can build just the kernel and the module quickly,
to see compile errors early:

```sh
jam -q kernel_x86_64 renguin
```

---

## 3. Get the Linux test programs

**Prebuilt:** use the `renguin-tests` archive from this session (all programs
already built and branded). Unpack it into `tests/linux/poc/` before building
the image, or copy it into the VM later.

**Or build them** on any Linux x86-64 machine with gcc and glibc's static
library (`libc6-dev` on Debian/Ubuntu):

```sh
cd /path/to/renguin/tests/linux/poc
make                 # builds and brands everything
./run-tests.sh .     # on Linux: 11 passed, 0 failed
```

| Program | What it exercises | Needs |
| --- | --- | --- |
| `hello` | `write`, `exit_group` | brand |
| `exit` | `exit` (single thread) | brand |
| `ebadf` | errno translation (`EBADF` = 9) | brand |
| `enosys` | unknown syscall → `ENOSYS` (38) | brand |
| `xmm` | vector registers survive a syscall | brand |
| `args` | argv/envp/auxv stack layout | brand |
| `mem` | `mmap`/`munmap`/`brk`/`uname` | brand |
| `fileio` | open/read/write/seek/stat, `fcntl`, `dup3`, `pipe2`, `getdents64`, `getcwd`, mkdir/unlink, `/proc/self/exe` | brand |
| `ids` | pids/tids, `prlimit64`, clocks, `nanosleep`, `getrandom`, `umask`, EFAULT on kernel pointers | brand |
| `libc` | **real static glibc**: printf, malloc, stdio files, exit status | nothing (glibc marks it Linux) |
| `libc-pie` | the same as a **static PIE** (ET_DYN, random load address) | nothing |

"brand" means the program has no Linux marker of its own, so `make` stamps
`EI_OSABI = Linux` into it with `tools/renguin-brand`, like FreeBSD's
`brandelf`. glibc programs carry a GNU ABI note and the GNU OS/ABI byte, which
Renguin recognizes on its own.

---

## 4. Boot it in VirtualBox

1. New VM → type **Other**, version **Other/Unknown (64-bit)**, 2 GB+ RAM,
   EFI **off**.
2. Storage → add `generated.x86_64/haiku.vmdk` as a SATA hard disk.
3. Optional but very useful: Settings → Serial Ports → enable Port 1, mode
   **Raw File**, path e.g. `renguin-serial.log`. Haiku writes kernel debug
   output there, which survives crashes.
4. Boot. If it reaches the desktop, the kernel patch does not break native
   Haiku: that is the first result worth reporting.

If it drops into the kernel debugger (KDL), type `bt` and photograph or copy
the screen; that backtrace is exactly what is needed.

---

## 5. Run the tests

Open Terminal:

```sh
cd ~/Desktop/renguin-tests
./hello ; echo $?            # expect: hello   then   42
./run-tests.sh .             # expect: 11 passed, 0 failed
./libc hi                    # expect: hello from libc, argc=2 hi  (exit 3)
grep -i "renguin\|personalit" /var/log/syslog
```

In the syslog you should see the personality load
(`foreign personality: loaded personalities/renguin/v1 (linux)` and
`renguin: Linux x86-64 personality loaded`). For any missing syscall you
see one line per program, such as
`renguin: team 1234: unimplemented Linux syscall 56 (clone)`.

Also check that native Haiku still behaves: open a few apps, run `ls`,
`pkgman search x`, build something small. Native programs never take Renguin's
paths. If they misbehave, that is a patch bug and high priority.

---

## 6. Dynamic programs: a Linux userland (optional, the exciting part)

Renguin can now load dynamically linked programs: it maps the program and its
ELF interpreter (`ld-musl` or `ld-linux`), and looks absolute paths up in a
Linux root first, like FreeBSD's `/compat/linux`. The root is
`/boot/system/non-packaged/compat/linux`.

The easiest userland to try is **Alpine's minirootfs** (about 3 MB, musl).
Download `alpine-minirootfs-<version>-x86_64.tar.gz` from
<https://alpinelinux.org/downloads/> (section "Mini root filesystem") in
WebPositive, then:

```sh
mkdir -p /boot/system/non-packaged/compat/linux
cd /boot/system/non-packaged/compat/linux
tar xzf ~/Downloads/alpine-minirootfs-*-x86_64.tar.gz
```

No reboot needed; Renguin checks for the root each time a program starts.
Alpine's `/bin/busybox` is a dynamically linked musl PIE, so these test the
interpreter loading, `mmap` of shared objects, and path translation:

```sh
L=/boot/system/non-packaged/compat/linux
$L/bin/busybox echo hello from Alpine
$L/bin/busybox uname -a                 # Linux haiku 6.1.0-renguin ...
$L/bin/busybox cat /etc/alpine-release  # read from inside the Linux root
$L/bin/busybox ls -l /etc               # getdents64 + stat; /etc is Alpine's
$L/bin/busybox sha256sum /etc/passwd
$L/bin/busybox pwd
```

Expected limits right now: no `fork`/`clone`/`execve` yet, so anything that
starts another program (`busybox sh -c ...`, `env`) fails with
`unimplemented Linux syscall`. Signal handlers are accepted but never run.
`ls /` shows the Linux root's `/`, as on FreeBSD; Haiku paths that don't exist
in the Linux root (such as `/boot/home`) still resolve to Haiku's own.
`/dev` always comes from Haiku.

A glibc userland (Debian or Ubuntu, for example from `debootstrap` on Linux)
works the same way: put it in the same directory and run its programs.

---

## 7. Iterating: updating only the module

The module lives in `/boot/system/non-packaged/add-ons/kernel/personalities/`,
not inside the read-only system package. So after a module-only change:

```sh
# on the build machine
jam -q renguin
# the result:
#   generated.x86_64/objects/haiku/x86_64/release/add-ons/kernel/personalities/renguin/renguin
```

Copy that file over
`/boot/system/non-packaged/add-ons/kernel/personalities/renguin` in the VM
and reboot. Changes to the kernel patch itself still need a new image.

If you boot with **"Disable user add-ons"** in the boot menu's safe mode
options, non-packaged add-ons are not loaded and Linux programs fall back
to "not an executable". That is a handy way to boot past a broken module.

---

## 8. What to report

- Did the patched image boot to the desktop? Do native apps work?
- Output of `./run-tests.sh .`
- `grep -i "renguin\|personalit" /var/log/syslog`
- For a crash or KDL: the backtrace (`bt`) and the last lines of the serial
  log.
- If you tried Alpine: which busybox commands worked and which did not, with
  their syslog lines.

---

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Linux program: "not an executable" / runtime_loader error | Module not loaded. Check the syslog for `foreign personality: loaded`; check that the file is in `/boot/system/non-packaged/add-ons/kernel/personalities/`; make sure safe mode "Disable user add-ons" is off. |
| A static musl program is not recognized | It has no Linux marker. Run `renguin-brand` on it. |
| `... needs the Linux interpreter ..., which is missing` in syslog | Dynamic program without a Linux root in place (section 6). |
| `git apply` fails | Haiku moved on. The hunks are small; apply them by hand and tell us which one moved. |
| `check-module.sh` fails | A Haiku API the module uses changed. Send the output. |

## What was verified, and how

- `tools/check-module.sh` compiles every module source, plus the patched
  `team.cpp`, `thread.cpp`, `arch_thread.cpp` and `foreign_personality.cpp`,
  against Haiku `master` headers in kernel mode (`-Wall -Wextra`, no
  warnings). It also generates `asm_offsets.h` the way Haiku's build does and
  assembles the patched `interrupts.S`.
- `tools/check-syscall-numbers.py` checks all 90 dispatched syscall numbers
  against FreeBSD's `syscalls.master`.
- All 11 test programs are built and pass on real Linux, establishing the
  expected results; `strace` confirms every syscall they make is
  implemented.
- **Not yet verified:** a full jam build with Haiku's cross-compiler, linking
  the module against `kernel.so`, and running on Haiku. That is this guide.
