# Haiku Linuxulator: Source-Level Research Report

Nov 5, 2026 · @Dalton

## Summary

A Haiku Linuxulator is feasible as a kernel personality modeled on FreeBSD's `sysentvec` design, but it needs four targeted Haiku kernel changes that FreeBSD never needed: an exec-time ABI hook, a per-thread syscall-table switch, a Linux TLS path, and a wider signal space. Everything else can live in a loadable module.

The seven findings that shape the whole plan:

1. **Haiku's kernel never inspects the program it runs.** `team_create_thread_start_internal()` in `src/system/kernel/team.cpp` always loads `/system/runtime_loader` with `elf_load_user_image()` and jumps to it; the target ELF is parsed only in userspace. Patching `runtime_loader` alone is not enough, because by then the team is already a Haiku-ABI team. Linux detection must happen in the kernel before `runtime_loader` is loaded, the way FreeBSD's `__elfN(get_brandinfo)()` does in `sys/kern/imgact_elf.c`.
2. **There is one 64-bit syscall door.** `IA32_MSR_LSTAR` points at `x86_64_syscall_entry` (`arch/x86/64/interrupts.S`), which indexes the single `kSyscallInfos` table. Linux binaries use the same `syscall` instruction, so the entry stub needs a per-thread table pointer, the equivalent of FreeBSD's `p->p_sysent->sv_table` lookup in `linux_fetch_syscall_args()`.
3. **Threads map well; process-level `CLONE_VM` does not.** `clone(CLONE_THREAD)` maps onto `thread_create_thread()` with `ThreadCreationAttributes::forkArgs`, which already starts a user thread from a copied register frame. FreeBSD implements `clone(CLONE_VM)` without `CLONE_THREAD` as `fork1(RFMEM)`; Haiku has no way for two teams to share one `VMAddressSpace`.
4. **Haiku writes its own TLS layout into every new thread.** `thread.cpp` stores `TLS_BASE_ADDRESS_SLOT`, `TLS_THREAD_ID_SLOT` and `TLS_USER_THREAD_SLOT` at `user_local_storage`, which is also the FS base. That would corrupt glibc's thread control block, so Linux threads need a separate path.
5. **Futexes can reuse Haiku's user-mutex machinery, not its semantics.** `locks/user_mutex.cpp` already has an address-keyed hash of `ConditionVariable`s, with physical-address keys for shared mappings via `vm_wire_page()`. FreeBSD's `linux_futex.c` sits on `umtx` the same way; Haiku needs a new futex queue built from those parts.
6. **Haiku has 40 signals and 8 real-time ones.** `signal_defs.h` sets `SIGNAL_REALTIME_MIN` 33 and `MAX_SIGNAL_NUMBER` 40; Linux has 64, and glibc reserves 32 and 33 for cancellation and `setxid`. This needs a kernel change.
7. **What we can reuse from FreeBSD is mostly data, not code.** Syscall numbers, errno and signal tables, flag constants and structure layouts transfer directly. Handler bodies call `kern_*()` functions and fill `td->td_retval`, so they must be rewritten against Haiku's `_user_*`/`vfs`/`vm` functions.

**Sources examined.** FreeBSD `main` at commit `c502df9eb193`, and Haiku `master` at commit `dd55696fdbd4`, both checked out on 2026-11-05. Every path and function named in this report was read in those trees unless marked otherwise.

**Convention.** **\[V\]** marks a fact verified in source. **\[H\]** marks a hypothesis or design proposal not yet tested. **\[U\]** marks something I could not verify.

## 1. FreeBSD Linuxulator architecture

The Linuxulator is an ABI personality: a `struct sysentvec` that replaces the syscall table, syscall-argument fetch, return-value encoding, signal delivery, stack layout and exec hooks for any process whose ELF image matched a Linux brand. Everything else (VFS, VM, sockets, scheduler) is FreeBSD's own, called through `kern_*()` functions. **\[V\]**

**Layout at `c502df9eb193` \[V\].** The user's path list is mostly correct, with three corrections. `linux_syscall.h`, `linux_syscalls.c`, `linux_sysent.c` and `linux_proto.h` are per-architecture and live in `sys/amd64/linux/`, generated from `syscalls.master`, not in `sys/compat/linux/`. `linux_mmap.c`, `linux_file.c`, `linux_event.c`, `linux_ipc.c` and `linux_rseq.c` are additional core files. `sys/x86/linux/` holds code shared by amd64 and i386. `sys/compat/linuxkpi/` is the Linux *driver* KPI for porting drivers like DRM into the FreeBSD kernel; it has nothing to do with running Linux binaries and is out of scope.

| Component | Problem it solves | How FreeBSD does it | FreeBSD dependencies | Haiku equivalent | Verdict |
| --- | --- | --- | --- | --- | --- |
| `amd64/linux/linux_sysvec.c` | Defines the Linux personality | `elf_linux_sysvec` fills `sv_table`, `sv_fetch_syscall_args`, `sv_set_syscall_retval`, `sv_sendsig`, `sv_setregs`, `sv_copyout_auxargs`, `sv_onexec`, `sv_thread_detach`. Three `Elf64_Brandinfo` entries (`linux_glibc2brand`, `linux_glibc2brandshort`, `linux_muslbrand`) are registered with `elf64_insert_brand_entry()` | `struct sysentvec`, `struct image_params`, trapframe, `pcb` | None. Haiku has no per-process ABI vector | Rewrite, keep the vtable idea |
| `amd64/linux/syscalls.master` | Single source of the syscall table | 366 entries up to `LINUX_SYS_MAXSYSCALL` 454; `STD` entries call `linux_*`, `NOPROTO` entries reuse the native syscall unchanged (for example `read`, `close`, `munmap`) | `makesyscalls` generator, `AUE_*` audit IDs | Haiku generates its table from `headers/private/system/syscalls.h` | Reuse the numbering and argument lists |
| `linux_sysvec.c: linux_fetch_syscall_args()` | Reads arguments | Args from `rdi rsi rdx r10 r8 r9`, number from `rax`, then `sa->callp = &p->p_sysent->sv_table[code]` | trapframe | `x86_64_syscall_entry` reads the same registers from the iframe | Adapt in assembly |
| `linux_sysvec.c: linux_set_syscall_retval()` | Linux return convention | `rax` = result, or `bsd_to_linux_errno()` (a negative number); `ERESTART` rewinds `rip` | errno table | Haiku stores the `_user_*` return value in `IFRAME_ax` | Adapt: map Haiku `status_t` to `-errno` |
| `linux_errno.c`, `linux_errno.inc` | Error numbers | Static tables `linux_errtbl[ELAST+1]` and `linux_to_bsd_errtbl[LINUX_ELAST+1]` | FreeBSD `ELAST` 97 | Haiku errors are large negative values from `B_GENERAL_ERROR_BASE` (`INT_MIN`) | Reuse the Linux side; build a Haiku table |
| `linux.c` | Signal-number and sockaddr translation | `linux_to_bsd_signal()`, `bsd_to_linux_signal()` with `LINUX_SIGTBLSZ` 31 and real-time offset math | FreeBSD `SIGRTMIN` | Haiku numbering differs (Haiku `SIGCHLD` 5, `SIGSTOP` 10) | Reuse the structure, new table |
| `linux_emul.c`, `linux_emul.h` | Per-thread and per-process Linux state | `struct linux_emuldata` (child set/clear TID pointers, `em_tid`, robust-futex head) hung off `struct thread`; `struct linux_pemuldata` (persona, ptrace flags, `oom_score_adj`) off `struct proc`; hooks `linux_proc_init()`, `linux_on_exec()`, `linux_on_exit()`, `linux_thread_dtor()` | `td_emuldata`, `p_emuldata`, `sx` locks, eventhandlers | No emuldata slot on `Thread`/`Team` | Rewrite; add slots |
| `linux_fork.c` | fork, vfork, clone, clone3, exit, `set_tid_address` | Non-thread clone becomes `fork1()` with `RFMEM` (`CLONE_VM`), `RFSIGSHARE` (`CLONE_SIGHAND`), `RFFDG` (no `CLONE_FILES`), `FR2_SHARE_PATHS` (`CLONE_FS`). `CLONE_THREAD` becomes `kern_thr_alloc()` + `thread_link()`. `linux_thread_detach()` clears `child_clear_tid` and calls `futex_wake()` | `fork1`, `rfork` flags, `kern_thr_alloc` | `fork_team()`; `thread_create_thread()` with `forkArgs` | Rewrite (section 8) |
| `linux_futex.c` | All futex ops and robust lists | Built on `umtx`: `umtx_key_get()`, `umtxq_sleep`, `umtxq_signal_mask()` for bitsets, `umtxq_requeue()`, `umtx_pi_*` for PI | `sys/umtxvar.h`, `kern_umtx.c` | `user_mutex.cpp` has keyed wait queues, not futex ops | Rewrite the logic, port the op decoder (section 9) |
| `linux_signal.c`, `x86/linux/linux_x86_sigframe.h`, `linux_sysvec.c: linux_rt_sendsig()` | Signals and frames | Translates `sigaction`/`sigset`; builds a Linux `rt_sigframe` with `ucontext` and FPU state; `linux_rt_sigreturn()` restores it | `ksiginfo`, `sigacts` | `setup_signal_frame()`, `arch_setup_signal_frame()` build a Haiku frame | Rewrite the frame code; reuse the layouts |
| `linux_mmap.c` | mmap, mprotect, madvise | `linux_mmap_common()` translates flags and calls `kern_mmap()`. `linux_madvise_dontneed()` emulates Linux zero-fill semantics | `vm_map` | `_user_map_file()`, `_user_set_memory_protection()`, `_user_memory_advice()` | Port the flag logic |
| `linux_misc.c` | brk, mremap, uname, sysinfo, prctl and more | `linux_brk()` uses `vmspace`; `linux_mremap()` only shrinks: growing returns `ENOMEM`/`EFAULT` | `vmspace`, `kern_munmap` | No `brk` syscall (libroot emulates it in `posix/unistd/brk.c`); no `mremap` | Rewrite |
| `linux_file.c`, `linux_stats.c`, `linux_getcwd.c` | Paths, open flags, stat, getdents, inotify | Flag translation, `struct l_newstat` conversion. `linux_inotify_init1()` reuses native inotify because event values match | `namei`, native inotify | `vfs.cpp` `_user_open()` etc.; Haiku node monitor, not inotify | Port the conversions |
| `linux_socket.c`, `linux_socket.h` | Socket ABI | Family, type, flag, sockopt and `cmsg` translation, including `SCM_RIGHTS` | `kern_sendit`, `mbuf` | `fs/socket.cpp` `_user_socket()`, `_user_sendmsg()` | Port the conversion tables |
| `linux_event.c` | epoll, eventfd, timerfd | epoll is a kqueue fd: `epoll_to_kevent()`, `kevent_to_epoll()` | kqueue | `events/event_queue.cpp` (kernel side of libbsd's `kqueue.cpp`) | Adapt the design |
| `linux_util.c` | `/compat/linux` overlay | `linux_emul_path` ("/compat/linux", sysctl `compat.linux.emul_path`); `linux_pwd_onexec()` calls `pwd_altroot()` so lookups try the overlay first | `pwd_altroot`, `namei` | `io_context::root` exists (chroot); no fallback root | Kernel change (section 10) |
| `linux_vdso.c`, `amd64/linux/linux_vdso_gtod.c` | vDSO for `clock_gettime` | A real ELF vDSO mapped into the shared page; `AT_SYSINFO_EHDR` points at it | shared page, `vdso_timehands` | Haiku commpage | Defer; optional |
| `compat/linprocfs/`, `compat/linsysfs/` | `/proc`, `/sys` | Pseudo-filesystems on FreeBSD's `pseudofs` | `pseudofs` | No procfs; `ramfs`, `bindfs` exist | Rewrite as a Haiku fs add-on |
| `linux_dummy.c` | Unimplemented calls | `DUMMY(name)` stubs that log once and return `ENOSYS` | `linux_msg` | none | Copy the pattern |

**Brand selection \[V\].** `__elfN(get_brandinfo)()` in `sys/kern/imgact_elf.c` tries four signals in order: a matching ELF note (`linux64_brandnote`, the GNU `NT_GNU_ABI_TAG`), the `EI_OSABI` byte, the `PT_INTERP` path, and finally the `kern.elf64.fallback_brand` sysctl. Each Linux brand lists its interpreter (`/lib64/ld-linux-x86-64.so.2`, `/lib/ld-musl-x86_64.so.1`). Musl static binaries carry no GNU note and usually `EI_OSABI` 0, which is why the fallback exists.

**Tests \[U\].** I found no Linuxulator test directory in `tests/sys/` at this commit. FreeBSD validates the Linuxulator largely with LTP run outside the tree; I could not confirm a current in-tree suite.

## 2. Haiku kernel architecture relevant to Linux compatibility

Haiku's x86-64 kernel already has most of the primitives a Linux personality needs, but it assumes in several places that every user team runs Haiku's own userland (`runtime_loader`, libroot, the commpage, Haiku's TLS slots). Those assumptions are where the kernel changes land.

| Subsystem | Files | Key functions and structures | What matters for Linux |
| --- | --- | --- | --- |
| Program start | `src/system/kernel/team.cpp` | `load_image_internal()`, `team_create_thread_start_internal()`, `exec_team()`, `user_space_program_args` | The kernel loads `B_SYSTEM_DIRECTORY/runtime_loader` and enters it with `thread_enter_userspace_new_team(thread, entry, programArgs, commpage)`. The target path is only passed as a string. **\[V\]** |
| Kernel ELF loader | `src/system/kernel/elf.cpp` | `elf_load_user_image()`, `verify_eheader()`, `elf_relocate()` | Maps the first segment at `B_RANDOMIZED_BASE_ADDRESS`, the rest at `B_EXACT_ADDRESS` relative to it, then relocates the image itself. Checks only magic, class and program-header fields; `PT_INTERP` is skipped. Not suitable for an `ET_EXEC` Linux binary at a fixed address. **\[V\]** |
| 32-bit compat scaffolding | `elf_compat.cpp`, `arch/x86/64/entry_compat.S`, `arch/x86/syscalls_compat.cpp`, `thread_types.h` | `elf32_load_user_image`, `x86_64_sysenter32_entry`, `kSyscallCompatInfos`, `THREAD_FLAGS_COMPAT_MODE` | Precedent for a second ABI with its own syscall table. `elf32_load_user_image` has no callers and `THREAD_FLAGS_COMPAT_MODE` is defined but unused in the checked-out tree, so the wiring is incomplete. **\[V\]** |
| Userspace loader | `src/system/runtime_loader/` | `runtime_loader()`, `load_program()`, `test_executable()`, `parse_elf_header()`, `pe_verify_header()` | `parse_elf_header()` checks neither `EI_OSABI` nor `e_machine`; `PT_INTERP` and `PT_NOTE` are explicitly ignored in `elf_load_image.cpp`. `test_executable()` already detects PE files and returns `B_UNKNOWN_EXECUTABLE`, a model for a Linux check. **\[V\]** |
| Syscall entry | `arch/x86/64/interrupts.S`, `arch/x86/64/syscalls.cpp`, `syscalls.cpp` | `x86_64_syscall_entry`, `init_syscall_registers()`, `kSyscallInfos`, `SYSCALL_COUNT`, `syscall_dispatcher()`, `register_generic_syscall()` | One `LSTAR` entry. Arguments are reloaded from `IFRAME_di/si/dx/r10/r8/r9` (the Linux register order), the function is called through `SYSCALL_INFO_function`, and `rax` is stored to `IFRAME_ax`. Return goes through `sysretq` (clobbers `rcx`, `r11`, as on Linux) or `iretq` when signals or restarts are pending. **\[V\]** |
| Teams | `headers/private/kernel/thread_types.h`, `team.cpp` | `struct Team`: `address_space`, `io_context`, `user_mutex_context`, `fSignalActions[MAX_SIGNAL_NUMBER]`, `commpage_address`, `user_data`, `parent`, `children`, `group_id`, `session_id` | A team owns exactly one `VMAddressSpace` and one `io_context`. The team ID is the main thread's ID (`Team::Create(thread->id, ...)`), which matches Linux's "pid = tid of the leader". **\[V\]** |
| Threads | `thread.cpp`, `headers/private/kernel/thread.h`, `arch/x86/arch_thread.cpp`, `arch/x86/64/thread.cpp` | `thread_create_thread()`, `ThreadCreationAttributes` (`forkArgs`, `stack_address`, `team`), `arch_store_fork_frame()`, `arch_restore_fork_frame()`, `arch_thread_init_tls()`, `x86_set_tls_context()` | `forkArgs` starts a user thread from a copied iframe with `ax = 0`. FS base comes from `thread->user_local_storage` on every context switch. On start, the kernel writes Haiku TLS slots into `user_local_storage`. **\[V\]** |
| fork | `team.cpp` | `fork_team()`, `vfs_new_io_context(parent, false)`, address-space copy | Classic fork into a new team; no flags for sharing memory, files or signal handlers. **\[V\]** |
| Signals | `signal.cpp`, `headers/private/system/signal_defs.h`, `ksignal.h`, `arch/x86/64/thread.cpp` | `send_signal_to_thread()`, `send_signal_to_team()`, `handle_signals()`, `setup_signal_frame()`, `arch_setup_signal_frame()`, `_user_restore_signal_frame()`, `_user_set_signal_stack()` | 40 signals, real-time 33-40; kernel-internal 62-64. The frame returns through a commpage stub (`COMMPAGE_ENTRY_X86_SIGNAL_HANDLER`). Linux needs `sa_restorer` and a Linux `rt_sigframe`. **\[V\]** |
| Virtual memory | `vm/vm.cpp`, `VMAddressSpace`, `VMArea`, `VMCache` | `vm_map_file()`, `vm_create_anonymous_area()`, `vm_resize_area()`, `_user_map_file()`, `_user_unmap_memory()`, `_user_set_memory_protection()`, `_user_memory_advice()`, `_user_sync_memory()`, `_user_mlock()`, `_user_munlock()`, `_user_reserve_address_range()`, `vm_wire_page()` | Area-based VM with POSIX mmap on top. No `mremap` and no kernel `brk`. Haiku `MAP_*` values differ from Linux (`MAP_ANONYMOUS` is 0x08 on Haiku, 0x20 on Linux). **\[V\]** |
| User-space locking | `locks/user_mutex.cpp`, `headers/private/kernel/user_mutex.h` | `UserMutexEntry` (address, `rw_lock`, `ConditionVariable`), `user_mutex_context`, `sSharedUserMutexContext`, `UserMutexContextFetcher` | Per-team table keyed by virtual address; shared table keyed by physical address after `vm_wire_page()`. The kernel interprets lock bits (`B_USER_MUTEX_LOCKED`, `B_USER_MUTEX_WAITING`), unlike a futex. **\[V\]** |
| VFS and descriptors | `fs/vfs.cpp`, `fs/fd.cpp`, `headers/private/kernel/vfs.h` | `io_context` (`root`, `cwd`, `fds`, `fds_close_on_exec`, `fds_close_on_fork`), `path_to_vnode()`, `vnode_path_to_vnode()`, `fd_and_path_to_vnode()`, `_user_change_root()` | The per-team `root` vnode supports chroot, but there is no "try the overlay, then fall back" lookup. `O_*` and `AT_*` values differ from Linux (`O_CREAT` 0x200 on Haiku, 0x40 on Linux). **\[V\]** |
| Events | `events/event_queue.cpp`, `events/wait_for_objects.cpp`, `headers/private/system/event_queue_defs.h`, `src/libs/bsd/kqueue.cpp` | `_user_event_queue_create()`, `_user_event_queue_select()`, `_user_event_queue_wait()`, `B_EVENT_LEVEL_TRIGGERED`, `B_EVENT_ONE_SHOT`; `_user_poll()`, `_user_select()` | A kernel event queue fd already backs libbsd's `kqueue`. This is the natural base for epoll, as kqueue is on FreeBSD. No eventfd, timerfd, signalfd or inotify in the kernel. **\[V\]** |
| Networking | `fs/socket.cpp`, `src/add-ons/kernel/network/stack/` | `_user_socket()`, `_user_bind()`, `_user_connect()`, `_user_accept()`, `_user_sendmsg()`, `_user_recvmsg()`, `_user_getsockopt()`, `_user_setsockopt()`, `_user_socketpair()` | Fd-based BSD sockets. Constants differ (`AF_INET6` 5 on Haiku, 10 on Linux; `SOCK_NONBLOCK` 0x40000 on Haiku). There is no `accept4` entry point in `socket.cpp`, so `_user_accept()` plus `fcntl` is the starting point. **\[V\]** |
| IPC | `posix/xsi_semaphore.cpp`, `posix/xsi_message_queue.cpp`, `posix/realtime_sem.cpp`, `port.cpp`, `sem.cpp` | `xsi_sem_context`, `realtime_sem_context` | System V semaphores and message queues exist in the kernel. I did not find System V shared memory there. **\[U\]** for shm |
| Filesystems | `src/add-ons/kernel/file_systems/` | `ramfs`, `bindfs`, `packagefs`, `userlandfs` | No procfs or sysfs. `bindfs` can expose `/dev` inside the Linux root. **\[V\]** |
| Random | `src/add-ons/kernel/bus_managers/random/` | `driver.cpp`, `yarrow_rng.cpp` | Kernel RNG for `getrandom`. **\[V\]** |

**Prior Haiku work \[V from the forum threads\].** The 2023 GSoC thread proposed two designs for non-native binaries: a kernel driver that receives syscalls from foreign teams, or a `runtime_loader` add-on that takes over when loading fails. PulkoMandy pointed out that epoll pulls in timerfd, eventfd and signalfd, and suggested kqueue as the better model; X512 said foreign binaries need kernel modules or a server for epoll, sysfs and futex. Nothing was decided. HyClone runs in the opposite direction (Haiku binaries on Linux): it patches `libroot` and `runtime_loader` to remove raw syscalls and uses a `hyclone_server` daemon for ports, semaphores and areas. The reusable lesson is its server pattern for state the host kernel lacks, which we should avoid by putting that state in the Haiku kernel instead.

## 3. FreeBSD-to-Haiku subsystem mapping

Of 16 subsystems, 4 need new Haiku kernel mechanisms (exec hook, syscall table switch, process-level `CLONE_VM`, signal range); the other 12 are translation layers over existing Haiku functions. Complexity is relative: S is days, M is one to three weeks, L is a month or more, XL is open-ended.

| Subsystem | FreeBSD source and functions | Haiku source and functions | Reuse | Required work | Size | Depends on |
| --- | --- | --- | --- | --- | --- | --- |
| ELF recognition | `kern/imgact_elf.c`: `__elfN(get_brandinfo)`, `__elfN(check_note)`; `amd64/linux/linux_sysvec.c`: `linux_glibc2brand`, `linux_muslbrand`, `linux64_brandnote` | `team.cpp`: `team_create_thread_start_internal()`, `load_image_internal()`, `exec_team()` | Concept, interpreter path list | New kernel hook: open the target, read the header, choose a personality before `runtime_loader` is loaded | M | — |
| Image activation | `kern/imgact_elf.c`: `__elfN(load_file)`, `__elfN(load_sections)`; `linux_copyout_strings`, `linux64_arch_copyout_auxargs()` | `vm/vm.cpp`: `vm_map_file()`, `vm_create_anonymous_area()`; `elf.cpp` (reference only) | Auxv layout, stack layout | New Linux loader: map `PT_LOAD` without relocation, honor `ET_EXEC` addresses, map `PT_INTERP`, build argv/envp/auxv | L | ELF recognition |
| Register setup | `linux_exec_setregs()` | `arch_thread_enter_userspace()`, `x86_initial_return_to_userland()` | Concept | Linux entry state: `rsp` at argc, `rdx` 0, FS base 0, clean FPU | S | Image activation |
| Syscall dispatch | `linux_fetch_syscall_args()`, `linux_set_syscall_retval()`, `syscalls.master`, `linux_sysent.c` | `interrupts.S`: `x86_64_syscall_entry`; `kSyscallInfos` | Numbers, argument lists | Per-thread table pointer in the entry stub; Linux table; return mapping | M | Exec hook sets the flag |
| Errno | `linux_errno.inc`: `linux_errtbl`, `linux_to_bsd_errtbl` | `headers/os/support/Errors.h` | Linux values | Haiku `status_t` → `-errno` function | S | — |
| Process emuldata | `linux_emul.c`: `em_find()`, `pem_find()`, `linux_proc_init()`, `linux_on_exec()`, `linux_on_exit()` | `struct Team`, `struct Thread` | Field list | Add an opaque ABI pointer to `Team` and `Thread`; lifecycle hooks in team and thread creation, exec, exit | M | — |
| Threads (`CLONE_THREAD`) | `linux_fork.c`: `linux_clone_thread()`, `linux_thread_detach()`, `linux_set_tid_address()` | `thread_create_thread()` with `forkArgs`; `arch_store_fork_frame()` | Flag validation | Build a fork frame with new `rsp`, `rax` 0, TLS; set/clear TID; futex wake on exit | M | Futex, TLS path |
| Processes (fork, vfork, clone without `CLONE_THREAD`) | `linux_fork()`, `linux_vfork()`, `linux_clone_proc()` (`RFMEM`, `RFSIGSHARE`, `RFFDG`, `FR2_SHARE_PATHS`) | `fork_team()`, `vfs_new_io_context()` | Flag validation | Plain fork and vfork are fine; `CLONE_VM`, `CLONE_FILES`, `CLONE_SIGHAND` across teams need shared-object refactoring | L–XL | Emuldata |
| exec | `linux_execve()`, `linux_common_execve()`, `linux_on_exec()` | `exec_team()`, `_user_exec()` | Concept | Exec of Linux from Linux and of Haiku from Linux must both switch personality | M | ELF recognition |
| Futex | `linux_futex.c` on `kern_umtx.c` | `locks/user_mutex.cpp` patterns, `ConditionVariable`, `vm_wire_page()` | Op decoding, `futex_atomic_op()` logic, robust-list walk | New futex queue with bitsets, requeue, wake-op, PI later | L | — |
| Signals | `linux_signal.c`, `linux.c` tables, `linux_rt_sendsig()`, `linux_rt_sigreturn()` | `signal.cpp`, `arch_setup_signal_frame()`, `_user_restore_signal_frame()` | Linux frame layout from `linux_x86_sigframe.h` | Number map, `sigset` conversion, Linux frame, `sa_restorer`; widen real-time range | L | Kernel signal change |
| Memory | `linux_mmap.c`, `linux_misc.c: linux_brk()`, `linux_mremap()` | `_user_map_file()`, `_user_unmap_memory()`, `_user_set_memory_protection()`, `vm_resize_area()` | Flag translation, `MADV_DONTNEED` approach | Kernel `brk`; `mremap` with move | M–L | — |
| Files and paths | `linux_file.c`, `linux_stats.c`, `linux_getcwd.c`, `linux_util.c: linux_pwd_onexec()` | `vfs.cpp`: `fd_and_path_to_vnode()`, `_user_open()`, `_user_read_stat()`, `io_context::root` | Flag and struct conversion | Overlay root lookup (a `pwd_altroot` equivalent) | M | — |
| Events | `linux_event.c` (epoll on kqueue, eventfd, timerfd) | `events/event_queue.cpp`, `wait_for_objects.cpp` | epoll-to-event design | epoll on `event_queue`; new eventfd and timerfd fd types | L | — |
| Networking | `linux_socket.c`, `linux_socket.h` | `fs/socket.cpp` `_user_*` socket calls | Constant and `cmsg` tables | Sockaddr, flag, sockopt, `SCM_RIGHTS` conversion; `accept4` | M–L | Files |
| /proc, /sys, /dev | `compat/linprocfs/linprocfs.c`, `compat/linsysfs/` | `file_systems/ramfs`, `bindfs` | File list only | New pseudo-filesystem add-on | L | Emuldata for per-process files |

## 4–5. Reusable FreeBSD code, licensing and dependencies

Reuse should be limited to Linux ABI *definitions* taken from BSD-2-Clause and BSD-3-Clause files, kept in a separate `linux_abi/` header directory with the original copyright notices. All handler logic should be rewritten. Haiku is MIT-licensed and its `License.md` says files may carry other licenses stated in their headers, so BSD-2 and BSD-3 code is acceptable with notices kept. **\[V\]**

**License census of the trees examined \[V\].** Across `sys/compat/linux/`, `sys/amd64/linux/`, `sys/x86/linux/`, `linprocfs` and `linsysfs`, SPDX tags show 49 BSD-2-Clause, 7 BSD-3-Clause and 2 BSD-4-Clause instances. The BSD-4-Clause files are `compat/linux/linux_futex.h` and `compat/linprocfs/linprocfs.c`; their advertising clause makes them unsuitable for Haiku, so do not copy from them. Files with no SPDX tag (`linux.c`, `linux_common.c`, `linux_mmap.c`, `linux_if.c` and others) carry plain two-clause BSD text in their headers, including `linux_errno.c` (Søren Schmidt, BSD two-clause text); check each file before copying. Generated files (`linux_sysent.c`, `linux_syscalls.c`, `linux_proto.h`) should be regenerated, not copied.

| Candidate | FreeBSD source | License | FreeBSD dependencies | Isolation work | Recommendation |
| --- | --- | --- | --- | --- | --- |
| Syscall numbers and argument lists | `amd64/linux/syscalls.master` | No header; covered by FreeBSD's top-level BSD-2 `COPYRIGHT` **\[V\]** | `makesyscalls` format, `AUE_*`, `NOPROTO` | Small: script converts to a Haiku table generator input; drop audit IDs | Reuse as data |
| Linux errno values and the reverse map | `compat/linux/linux_errno.h`, `linux_errno.inc` | BSD-2 | FreeBSD `E*` on the left side | Small: keep the `LINUX_E*` column, rebuild the Haiku side | Reuse Linux values; new table |
| Signal constants and frame layout | `compat/linux/linux.h` (`LINUX_SIG*`), `x86/linux/linux_x86_sigframe.h` | BSD-2; BSD-3 | `l_*` typedefs | Small | Reuse |
| Linux structure definitions (`l_newstat`, `l_sigaction`, `l_timespec`, `l_clone_args`, `l_user_clone_args`, `epoll_event`) | `amd64/linux/linux.h`, `compat/linux/linux_fork.h`, `linux_event.h`, `linux_time.h` | BSD-2 | FreeBSD integer typedefs | Small: map `l_long` etc. to fixed-width types | Reuse |
| Flag constants (`LINUX_O_*`, `LINUX_MAP_*`, `LINUX_CLONE_*`, `LINUX_FUTEX_*`, socket and sockopt values) | `compat/linux/linux_file.h`, `linux_mmap.h`, `linux_fork.h`, `linux_socket.h` (BSD-3), futex values (also in BSD-4 `linux_futex.h`) | BSD-2, BSD-3; futex header BSD-4 | none | Small. Rewrite the futex constants from the Linux UAPI numbers instead of copying `linux_futex.h` | Reuse, except futex header |
| Conversion routines (signal-number tables, `sigset` conversion, open-flag translation, stat conversion) | `linux.c`, `linux_signal.c`, `linux_file.c`, `linux_stats.c` | BSD-2 | FreeBSD constants on one side; `struct stat` | Medium: the logic is reusable, the native side changes | Adapt |
| `futex_atomic_op()` and the op dispatcher | `linux_futex.c` | BSD-2 | `umtx_key`, `umtxq_*`, `td_retval` | Medium: the op decoder is standalone; the waiting logic is not | Adapt the decoder, rewrite the queue |
| epoll translation logic | `linux_event.c` | BSD-2 | kqueue `struct kevent`, `kern_kevent_fp` | Medium | Rewrite against `event_queue`, borrowing the mapping |
| Socket option and `cmsg` translation | `linux_socket.c` | BSD-2 | `mbuf`, `kern_sendit`, `sockaddr` helpers | Large | Port tables; rewrite I/O paths |
| Everything in `linux_fork.c`, `linux_emul.c`, `linux_sysvec.c`, `linux_misc.c` | as named | BSD-2 / BSD-3 | `struct proc`, `struct thread`, `fork1`, `vmspace`, `pcb` | Not separable | Architectural reference only |
| `linprocfs` | `compat/linprocfs/linprocfs.c` | BSD-4 | `pseudofs` | — | Do not copy; use the file list only |

**Dependency rule.** Copy a FreeBSD file only if, after removing its `#include <sys/...>` lines, it compiles with nothing but fixed-width integer types. That leaves headers and lookup tables. Handler bodies fail this test because they use `struct thread *td`, `td->td_retval[]`, `kern_*()` calls and FreeBSD locks.

**Upstream acceptance \[U\].** I have not checked whether Haiku maintainers would accept a Linux personality in-tree; this needs a discussion on the Haiku development list before work beyond the proof of concept.

## 6. Linux ELF loading and ABI selection

**Proposal \[H\]:** add one hook at the top of `team_create_thread_start_internal()` that asks registered personalities to probe the target file; if the Linux personality claims it, the kernel loads the Linux image and its interpreter itself and never loads `runtime_loader`. Native Haiku teams take the existing path unchanged.

**Why here \[V\].** Both `load_image_internal()` (spawn) and `exec_team()` (exec) end in `team_create_thread_start()`, so one hook covers every way a program starts. At that point the old address space is gone (`vm_delete_areas()` in `exec_team()`), the commpage has not been cloned yet and the stack area with `user_space_program_args` is already built. Doing this in `runtime_loader` instead would mean a Haiku-ABI process loading a foreign image and then asking the kernel to switch its ABI, which leaves Haiku's TLS slots, commpage and libroot-era state behind in the Linux process.

**Haiku userland needs no change to launch Linux programs \[V\].** When a Haiku shell calls `exec`, libroot's `__test_executable()` calls `runtime_loader`'s `test_executable()`, which accepts any 64-bit ELF with a non-zero entry point because `parse_elf_header()` does not check `EI_OSABI` or `PT_INTERP`. The request therefore reaches the kernel, where the hook decides.

**Probe order**, adapted from `__elfN(get_brandinfo)()`:

1. Reject unless `ELFCLASS64`, `EM_X86_64` and `ET_EXEC` or `ET_DYN` with a non-zero entry.
2. `PT_INTERP` equals a registered Linux interpreter (`/lib64/ld-linux-x86-64.so.2`, `/lib/ld-musl-x86_64.so.1`, and FreeBSD's short form `/lib64/ld-linux.so.2`) → Linux.
3. `PT_INTERP` is Haiku's `runtime_loader` → native. **\[U\]** I did not confirm the exact interpreter string Haiku's toolchain writes; check with `readelf -l` on a Haiku binary.
4. `EI_OSABI` is `ELFOSABI_LINUX` (3) → Linux.
5. A `PT_NOTE` containing `NT_GNU_ABI_TAG` with OS field 0 (Linux) → Linux. This catches glibc static binaries.
6. Anything else → native. Static musl binaries fall here; they need an explicit opt-in, such as a `linux` launcher command or a file attribute. FreeBSD solves the same gap with its `fallback_brand` sysctl, but making Linux the default would risk misreading native binaries.

**The Linux loader is new code, not `elf_load_user_image()` \[V for the reason\].** Haiku's kernel loader maps the first segment at a random base and then relocates the image itself, which is right for `runtime_loader` and wrong here: `ET_EXEC` binaries need exact addresses and `ld-linux` relocates itself. The Linux activator should:

1. Map each `PT_LOAD` of the executable with `vm_map_file()` (file-backed, `REGION_PRIVATE_MAP`) at `p_vaddr` for `ET_EXEC`, or at a random base plus `p_vaddr` for `ET_DYN`; zero the partial page after `p_filesz` and cover bss with `vm_create_anonymous_area()`.
2. Resolve `PT_INTERP` inside the Linux root (section 10), map it the same way at its own random base, and use its entry point.
3. Rebuild the stack in Linux format, top-down: strings, 16 random bytes for `AT_RANDOM`, platform string, the auxv, envp, argv and argc at `rsp`. Required auxv entries: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`, `AT_ENTRY`, `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`, `AT_SECURE`, `AT_RANDOM`, `AT_HWCAP`, `AT_HWCAP2`, `AT_CLKTCK`, `AT_PLATFORM`, `AT_EXECFN`. `AT_SYSINFO_EHDR` is optional; glibc and musl fall back to real syscalls without a vDSO **\[H\]**, which lets us defer the vDSO.
4. Set entry state as `linux_exec_setregs()` does: `rip` = interpreter entry, `rsp` = argc, all other general registers 0, FS and GS base 0, `rflags` user default, fresh FPU state. On Haiku that means a Linux variant of `arch_thread_enter_userspace()` that does not pass `programArgs` and `commpage` in `rdi`/`rsi`.
5. Skip `clone_commpage_area()` for Linux teams, and do not let `thread.cpp` write Haiku TLS slots (section 8).

The existing stack area already holds the flat argv/envp copied by `team_create_thread_start_internal()`, so the Linux activator reads them from `teamArgs` instead and lays out the Linux stack in the same `user_stack_base` area.

**Thread-local storage.** Linux TLS is `PT_TLS` handled by `ld-linux` (or by the static libc's startup), which calls `arch_prctl(ARCH_SET_FS)`. The kernel only has to store that value in `thread->user_local_storage` so `x86_set_tls_context()` reloads it on every switch. **\[V for the mechanism\]**

## 7. Linux syscall dispatch

**Proposal \[H\]:** a new thread flag, set on every thread of a Linux team, sends `x86_64_syscall_entry` down a separate branch that calls a C dispatcher with the iframe. Haiku threads pay one extra `testl`. This is FreeBSD's `p_sysent` idea, implemented as a branch because Haiku's table lookup is hard-coded in assembly.

**What the existing entry does \[V\].** `x86_64_syscall_entry` (`arch/x86/64/interrupts.S`) saves `rax` into `orig_rax` and `r14`, checks `r14` against `SYSCALL_COUNT`, indexes `kSyscallInfos` (16-byte entries), copies stack arguments when `parameter_size` exceeds 48 bytes, reloads `rdi, rsi, rdx, r10→rcx, r8, r9` from the iframe and calls `SYSCALL_INFO_function`. On return it stores `rax` in `IFRAME_ax` and leaves through `sysretq`, or through `.Lpost_syscall_work` and `iretq` when signals, debugging or restarts are pending.

**Three traps in that return path \[V\], each needing a Linux-aware branch:**

1. `cmpq $SYSCALL_RESTORE_SIGNAL_FRAME, %r14` picks the full-restore path by *Haiku* syscall number. A Linux call that happens to share that number would take the wrong path, and Linux `rt_sigreturn` (15) needs that path but has a different number.
2. `CLEAR_FPU_STATE()` zeroes `xmm0`–`xmm15` before `sysretq`. Haiku can do this because its syscalls are called through function stubs, where vector registers are caller-saved. Linux preserves them across `syscall`, and glibc, musl, Go and Rust issue inline `syscall` instructions that only declare `rcx`, `r11` and memory as clobbered. Linux threads must skip the clear. **\[U\]** I have not confirmed whether the kernel can touch user vector state during a syscall without saving it; until that is verified, the Linux path should save user FPU state at entry and restore it on exit.
3. `x86_restart_syscall()` rewinds `ip` by 2 and reloads `rax` from `orig_rax`. That works for Linux too, since `syscall` is 2 bytes, but Linux restart semantics depend on `SA_RESTART` and `-ERESTART*` codes, so the Linux dispatcher must decide when to set `THREAD_FLAGS_RESTART_SYSCALL`.

**Dispatcher design:**

```
x86_64_syscall_entry:
    ... existing prologue, iframe built, r12 = Thread* ...
    testl  $THREAD_FLAGS_FOREIGN_ABI, THREAD_flags(%r12)
    jnz    .Lforeign_syscall          // new
    ... unchanged Haiku path ...
.Lforeign_syscall:
    movq   %rbp, %rdi                 // iframe*
    call   *gForeignSyscallHook       // set by the Linux module
    jmp    .Lforeign_return           // FPU kept, iret or sysret
```

```
extern "C" void linux_syscall_dispatch(iframe* frame)
{
    uint64 nr = frame->orig_rax;
    const linux_sysent* e = nr < LINUX_SYS_MAX ? &kLinuxSysent[nr] : &kNosys;
    int64 r = e->fn(frame->di, frame->si, frame->dx,
                    frame->r10, frame->r8, frame->r9);
    frame->ax = r;   // handlers return a value or -LINUX_Exxx
}
```

The table should be generated from FreeBSD's `amd64/linux/syscalls.master`, which gives numbers and argument lists for all 454 slots. Each handler returns a Linux value directly (`-LINUX_ENOENT`, not `B_ENTRY_NOT_FOUND`); a single `haiku_to_linux_errno(status_t)` maps results from `_user_*`/`vfs`/`vm` calls, mirroring `bsd_to_linux_errno()` in `linux_errno.c`. Haiku errors are values such as `B_POSIX_ERROR_BASE` offsets from `INT_MIN`, so the map is a switch, not an array.

**Calling Haiku's `_user_*` functions from handlers.** Most `_user_*` functions take user pointers and check them (`IS_USER_ADDRESS`, `user_memcpy`). Linux handlers can pass converted structures only if they are in user memory, so converters should either call the kernel-internal versions (for example `common_open`-style helpers in `vfs.cpp`) or copy into a kernel buffer and use the `_kern_*` variants. Which internal helpers are exported from `vfs.cpp` for an add-on needs per-call checking. **\[U\]**

**Module boundary.** The hook pointer and the flag live in the kernel; the table and handlers can live in a kernel module that registers itself, the way FreeBSD's `linux64_elf_modevent()` calls `elf64_insert_brand_entry()`. Haiku's `register_generic_syscall()` is not a fit: it routes through `_user_generic_syscall()` with a subsystem name and a buffer, which no Linux binary will call.

**Unimplemented calls** return `-ENOSYS` and log once per call number per team, like FreeBSD's `DUMMY()` macro in `linux_dummy.c`.

## 8. Process and thread compatibility

**Proposal \[H\]:** support the two clone shapes real programs use first, and reject the rest with `EINVAL` until a later stage. The pthread shape (`CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID`) becomes a new thread in the same team. The fork shape (`SIGCHLD` only, or `CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD` from glibc's `fork()`) becomes `fork_team()`. `vfork`/`CLONE_VM|CLONE_VFORK` (used by `posix_spawn`) becomes fork plus parent suspension.

FreeBSD has the same split: `linux_clone()` sends `CLONE_THREAD` to `linux_clone_thread()` and everything else to `linux_clone_proc()`. The difference is that FreeBSD's `fork1()` accepts `RFMEM`, `RFSIGSHARE` and `RFFDG` to share memory, signal handlers and file tables between processes, while Haiku's `fork_team()` always copies.

| Flag | Linux meaning | FreeBSD (`linux_fork.c`) | Haiku plan |
| --- | --- | --- | --- |
| `CLONE_THREAD` | Same thread group | `kern_thr_alloc()` + `thread_link()` | `thread_create_thread()` with `team` = current and `forkArgs` = copied iframe **\[V the mechanism exists\]** |
| `CLONE_VM` | Share address space | `RFMEM` | Implied by a thread. Without `CLONE_THREAD`: only the vfork case, done as fork with the parent blocked until exec or exit. True sharing between teams would require `Team::address_space` to be reference-counted and shared, an XL kernel change **\[H\]** |
| `CLONE_FS` | Share cwd, root, umask | `FR2_SHARE_PATHS` | Implied by a thread (one `io_context` per team). Across teams: not supported at first |
| `CLONE_FILES` | Share fd table | absence of `RFFDG` | Implied by a thread. Across teams: `io_context` is already reference-counted (`ref_count`), so sharing may be feasible **\[H\]** |
| `CLONE_SIGHAND` | Share handlers | `RFSIGSHARE` | Implied by a thread (`Team::fSignalActions`). Required with `CLONE_THREAD` |
| `CLONE_PARENT` | Sibling, not child | Reparent after `fork1()` | Reparent in `fork_team()`; rejected with `CLONE_THREAD`, as FreeBSD does |
| `CLONE_SETTLS` | Set FS base | `linux_set_cloned_tls()` | Set `thread->user_local_storage` before the thread first runs |
| `CLONE_PARENT_SETTID` | Write TID to parent memory | `copyout` after creation | `user_memcpy` in the parent before return |
| `CLONE_CHILD_SETTID` | Write TID in child memory | stored in emuldata, written in `linux_schedtail()` | Write before the child returns to user mode |
| `CLONE_CHILD_CLEARTID` | On exit, zero TID word and futex-wake it | `linux_thread_detach()`: `suword32(child_clear_tid, 0)` then `futex_wake()` | Thread-exit hook does the same; glibc's `pthread_join` depends on it |

**What has to change in Haiku's thread start \[V for the problem\].** When a thread starts, `thread.cpp` writes `TLS_BASE_ADDRESS_SLOT`, `TLS_THREAD_ID_SLOT` and `TLS_USER_THREAD_SLOT` at `thread->user_local_storage`, and for fork frames it even asserts that `userTls[TLS_BASE_ADDRESS_SLOT] == user_local_storage`. `arch_thread_init_tls()` sets `user_local_storage` to the top of the user stack area. For a Linux thread, `user_local_storage` must be the glibc TCB pointer from `CLONE_SETTLS`, and the kernel must write nothing there. The Linux path needs a flag that skips that block, and `arch_prctl(ARCH_SET_FS)` must update `user_local_storage` and the MSR together.

**Thread stacks.** Linux passes the child stack in the `clone` arguments; `thread_create_thread()` creates its own stack area when `thread->user_stack_base` is 0. For `CLONE_THREAD`, the fork frame's `rsp` is set to the caller's stack, and the kernel should not allocate one. The cleanest way is a `ThreadCreationAttributes` field that marks the stack as caller-provided. **\[H\]**

**IDs.** Haiku's team ID equals its main thread's ID, and thread IDs are global, so `getpid()` = team ID, `gettid()` = thread ID and `getppid()` = parent team ID map directly. **\[V\]** Thread IDs and team IDs share one space, as Linux TIDs and PIDs do.

**Exit.** `exit` (60) ends one thread: Haiku `thread_exit()` plus the clear-TID hook and robust-list walk. `exit_group` (231) ends the team: the `_user_exit_team()` path. Linux exit status goes to the parent through `wait4`/`waitid`, which can sit on Haiku's `wait_for_child()` in `team.cpp`, with status encoding translated.

**Signals to threads.** `tgkill(tgid, tid, sig)` maps to `send_signal_to_thread_id()` after checking that the thread belongs to team `tgid`. `kill` maps to `send_signal_to_team_id()` or the process-group path.

**Signals overall.** Haiku has 40 signals (`MAX_SIGNAL_NUMBER`), with real-time signals 33–40, and kernel-internal signals at 62–64 (`ksignal.h`). Linux has 64, real-time 32–64, and glibc uses 32 for thread cancellation and 33 for `setxid` broadcasts. Without a wider range, `pthread_cancel` and `setuid` in threaded programs break. Options: raise `SIGNAL_REALTIME_MAX` toward 61 for all teams (a change to Haiku's `sigset` users and `fSignalActions` sizing), or keep a Linux-only extension. This needs a decision with Haiku developers. **\[H\]**

**Linux signal frames.** `rt_sigaction` requires `SA_RESTORER`; the kernel builds a Linux `rt_sigframe` (`siginfo`, `ucontext`, FPU state) on the user or alternate stack and jumps to the handler with the restorer as return address. FreeBSD's `linux_rt_sendsig()` and `linux_rt_sigreturn()` in `amd64/linux/linux_sysvec.c` are the reference; on Haiku this becomes a Linux variant of `arch_setup_signal_frame()` chosen by the team's personality, plus a `rt_sigreturn` handler that leaves through the full-restore path.

## 9–10. Futex feasibility and memory management

**Futexes are feasible without changing Haiku's existing user-mutex API** by adding a new futex queue to the Linux module that reuses the same building blocks: an `OpenHashTable` of entries keyed by address, a `ConditionVariable` per waiter, private keys by virtual address and shared keys by physical address under `vm_wire_page()`. **\[H, building blocks V\]**

**Why not call `_user_mutex_*` directly \[V\].** `user_mutex_prepare_to_lock()` and `user_mutex_unblock()` set and clear `B_USER_MUTEX_LOCKED` and `B_USER_MUTEX_WAITING` in the user word. A futex word belongs to userland: glibc stores its own states (0, 1, 2) and the kernel may only compare it. Using user mutexes would corrupt glibc's lock word.

**What FreeBSD does \[V\].** `linux_futex()` dispatches on the op. `FUTEX_WAIT`/`WAIT_BITSET` compare the word and sleep on a `umtx` queue; `WAKE`/`WAKE_BITSET` use `umtxq_signal_mask()`, which matches bitsets; `CMP_REQUEUE` uses `umtxq_requeue()`; `WAKE_OP` decodes the op with `futex_atomic_op()`; the PI ops use `umtx_pi_*`. Keys come from `umtx_key_get()`, which distinguishes private and shared mappings.

**Haiku futex queue, by operation:**

| Op | Priority | Haiku implementation | Notes |
| --- | --- | --- | --- |
| `FUTEX_WAIT`, `FUTEX_WAIT_BITSET` (+ `PRIVATE`, `CLOCK_REALTIME`) | P0 | Lock bucket, `user_memcpy` the word, compare, add waiter with bitset, unlock, `ConditionVariableEntry::Wait()` with timeout; return `-EAGAIN`, `-ETIMEDOUT`, `-EINTR` | Absolute timeout for `WAIT_BITSET`, relative for `WAIT` |
| `FUTEX_WAKE`, `FUTEX_WAKE_BITSET` | P0 | Wake up to `n` waiters whose bitset intersects | Returns count woken |
| `FUTEX_CMP_REQUEUE` | P1 | Compare `val3`, wake `n`, move up to `n2` waiters to the second key | glibc condition variables |
| `FUTEX_WAKE_OP` | P1 | Port `futex_atomic_op()` decoding; do the atomic op with fault handling on user memory | Older glibc condition variables |
| `FUTEX_REQUEUE` (deprecated) | P2 | As `CMP_REQUEUE` without the compare | FreeBSD only enables this for musl (`LINUX_BI_FUTEX_REQUEUE`) |
| `FUTEX_LOCK_PI`, `UNLOCK_PI`, `TRYLOCK_PI`, `WAIT_REQUEUE_PI`, `CMP_REQUEUE_PI` | P3 | Owner TID in the word; needs priority inheritance in Haiku's scheduler, which I did not find **\[U\]** | Return `-ENOSYS` at first; only `PTHREAD_PRIO_INHERIT` mutexes use them |
| `set_robust_list`, `get_robust_list`, exit-time list walk | P1 | Store the head per thread; on exit walk the list, set `FUTEX_OWNER_DIED`, wake one | Port the walk from `release_futexes()` in `linux_futex.c` |

**Performance.** The uncontended path never enters the kernel. The contended path costs one hash lookup and a condition-variable wait, similar to Haiku's own `user_mutex` path, so performance should be comparable to native Haiku mutexes. **\[H\]** Waking threads one by one has to avoid the thundering-herd pattern on `FUTEX_WAKE` with `INT_MAX`.

**Memory management.** Most mmap semantics map onto Haiku's area-based VM, but `brk`, `mremap` and some `MAP_*` flags need new code.

| Linux operation | Haiku basis | Gap | Kernel change? |
| --- | --- | --- | --- |
| `mmap` `MAP_PRIVATE`/`MAP_SHARED`, file or `MAP_ANONYMOUS` | `_user_map_file()` / `vm_map_file()` with `REGION_PRIVATE_MAP` or `REGION_NO_PRIVATE_MAP` | Flag values differ (Haiku `MAP_ANONYMOUS` 0x08, Linux 0x20); translate | No |
| `MAP_FIXED` | `B_EXACT_ADDRESS` with unmap of the overlapped range | Linux silently replaces existing mappings | No **\[H\]**, check `vm_map_file()` replacement behavior |
| `MAP_FIXED_NOREPLACE`, `MAP_GROWSDOWN`, `MAP_POPULATE`, `MAP_NORESERVE`, `MAP_STACK` | `B_EXACT_ADDRESS` without unmap; populate by touching | `GROWSDOWN` has no Haiku equivalent; treat as a normal mapping | No |
| `munmap`, `mprotect` | `_user_unmap_memory()`, `_user_set_memory_protection()` | Per-page protection inside one area must work | No |
| `madvise` | `_user_memory_advice()` | `MADV_DONTNEED` must zero private anonymous pages immediately; FreeBSD emulates this in `linux_madvise_dontneed()` | Maybe |
| `msync`, `mlock`, `munlock` | `_user_sync_memory()`, `_user_mlock()`, `_user_munlock()` | `MS_*` and `MCL_*` translation | No |
| `brk` | None in kernel; libroot emulates it in userspace (`posix/unistd/brk.c`) | Linux libcs call the syscall. Implement with a reserved range after the highest `PT_LOAD` and `vm_resize_area()` on one anonymous area | In module |
| `mremap` | `vm_resize_area()` for whole areas | No `mremap`. Shrink = unmap the tail. Grow in place = resize if the range is a whole area with free space after it. `MREMAP_MAYMOVE` = new area plus moving the pages | Yes for an efficient move. FreeBSD only supports shrinking, and glibc's `realloc` falls back to copy on failure, so P2 |
| Shared anonymous memory across `fork` | Area inheritance in `fork_team()` | Verify that \`MAP\_SHARED | MAP\_ANONYMOUS\` stays shared in the child |
| System V shm (`shmget`, `shmat`) | Not found in `src/system/kernel/posix/` | Needs an implementation on areas | **\[U\]** |

## 11. Dynamic linking, glibc and the Linux runtime tree

**Proposal \[H\]:** run unmodified Linux userlands (Alpine's musl tree first, then a Debian glibc tree) from a per-team overlay root, exactly as FreeBSD does. `ld-linux-x86-64.so.2` does all symbol resolution, versioning and TLS setup; the kernel only loads it, supplies auxv and resolves paths. Haiku's libroot and `runtime_loader` are never mapped into a Linux team, so the proposed `src/system/libroot/compat/linux/` tree is not needed.

**How FreeBSD does it \[V\].** `linux_emul_path` defaults to `/compat/linux` (sysctl `compat.linux.emul_path` in `linux_util.c`). On every Linux exec, `linux_pwd_onexec()` looks the path up and calls `pwd_altroot()`, which makes `namei()` try each absolute path under the overlay first and fall back to the real root. The kernel also resolves `PT_INTERP` through the same lookup, so `/lib64/ld-linux-x86-64.so.2` is found inside the overlay. Linux userlands come from packages (`linux_base-rl9`) or `debootstrap`. `uname` reports `Linux` and `5.15.0` by default (`linux_mib.c`, `LINUX_KVERSION` 5, `LINUX_KPATCHLEVEL` 15); some glibc builds check the kernel version.

**Haiku version.** Add an alternate-root vnode to the Linux team state and teach `vfs.cpp`'s absolute-path lookup (`path_to_vnode()` / `fd_and_path_to_vnode()`) to try it first when the calling team is Linux, with fallback to the normal root on `B_ENTRY_NOT_FOUND`. **\[H\]** Points to settle:

- **Location.** Haiku's `/` is the in-memory `rootfs` (`fs/rootfs.cpp`), so a `/compat/linux` created at the root does not survive a reboot. The tree should live on the boot volume, for example under `/boot/system/non-packaged/`, with a kernel setting for the path. **\[H\]**
- **Creation semantics.** FreeBSD's rule for files created through the overlay (does `open("/etc/foo", O_CREAT)` create inside the overlay or the real root?) needs to be read in `vfs_lookup.c`, which I did not inspect. **\[U\]**
- **Escapes.** `..` above the overlay root, symlinks with absolute targets and `/proc/self/root` all need tests, because a fallback lookup can expose host paths that a chroot would hide. That is intended (Linux programs should see `/boot/home`), but must be deliberate.

**Minimum tree for a musl hello-world and a shell:** `/lib/ld-musl-x86_64.so.1`, `/bin/busybox` and links, `/etc/passwd`, `/etc/group`, `/etc/resolv.conf`. For glibc, add `/lib64/ld-linux-x86-64.so.2`, `/lib/x86_64-linux-gnu/libc.so.6` and friends, `/etc/ld.so.cache` (optional), `/etc/nsswitch.conf`, and locale data if programs call `setlocale`.

**Kernel services glibc's startup calls**, which together set the Stage 5 syscall floor: `brk`, `arch_prctl(ARCH_SET_FS)`, `set_tid_address`, `set_robust_list`, `rseq` (may return `-ENOSYS`; glibc then disables rseq), `prlimit64`, `mmap`, `mprotect`, `munmap`, `openat`, `read`, `pread64`, `close`, `newfstatat`, `access`/`faccessat`, `getrandom` (malloc initialization in recent glibc), `readlink("/proc/self/exe")` for `$ORIGIN`, `uname`, `exit_group`. **\[H\]** confirm by tracing a static and a dynamic binary under real Linux with `strace -f` before Stage 5.

**Symbol versioning and TLS** are handled entirely by `ld-linux` and libc in userspace; the kernel's only TLS duty is `arch_prctl` and `CLONE_SETTLS` (section 8).

**vDSO.** Not required at first. Without `AT_SYSINFO_EHDR`, glibc and musl call `clock_gettime`, `gettimeofday`, `time` and `getcpu` as syscalls. Later, a small vDSO ELF built from Haiku's commpage time data (FreeBSD's model: `linux_vdso.c`, `amd64/linux/linux_vdso_gtod.c`) can be mapped like FreeBSD's shared page.

**/proc, /sys, /dev.** glibc and many programs read `/proc/self/exe`, `/proc/self/maps`, `/proc/self/fd/`, `/proc/cpuinfo`, `/proc/meminfo`, `/proc/sys/kernel/*`, `/sys/devices/system/cpu/online` (glibc's `sysconf(_SC_NPROCESSORS_ONLN)`). A new `linprocfs`-style Haiku filesystem add-on mounted inside the overlay should provide them, driven by `Team`/`VMArea` iteration. `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom`, `/dev/tty` and the pty devices can be reached by mounting Haiku's `/dev` inside the overlay with `bindfs`. **\[H\]** check that Haiku's device names match (`drivers/common/null.cpp`, `bus_managers/random`).

**Packages and Flatpak (later).** Debian packages need only a working dpkg inside the overlay. Flatpak needs user and mount namespaces, `bwrap`, FUSE and seccomp, none of which have a Haiku basis; treat it as a separate project after Stage 8.

## 12. Prioritized x86-64 syscall matrix

Only three calls are P0: \`write\`, \`exit\` and \`exit\_group\`, enough for the proof of concept. P1 covers static musl programs and glibc startup, P2 threads, events and networking, P3 the rest. Most calls are translations over existing Haiku functions; only \`arch\_prctl\`, \`rt\_sigreturn\` and \`clone\` need changes to Haiku's own kernel. Numbers and FreeBSD handlers come from `amd64/linux/syscalls.master` **\[V\]**; Haiku functions are verified names **\[V\]**; the mapping judgement is **\[H\]**.

Mapping: **Direct** = call the Haiku function with converted arguments and errno. **Translate** = convert flags, structures or semantics. **New** = needs a new mechanism (module or kernel). **K** marks a change to Haiku's own kernel source.

| # | Call | FreeBSD handler | Haiku basis | Mapping | Pri |
| --- | --- | --- | --- | --- | --- |
| 1 | write | `linux_write` | `_user_write` (pos -1) | Direct | P0 |
| 60 | exit | `linux_exit` | `thread_exit()` + clear-TID hook | Translate | P0 |
| 231 | exit\_group | `linux_exit_group` | `_user_exit_team` | Direct | P0 |
| 0 | read | native | `_user_read` | Direct | P1 |
| 3 | close | native | `_user_close` | Direct | P1 |
| 8 | lseek | `linux_lseek` | `_user_seek` | Translate (whence) | P1 |
| 19 | readv | native | `_user_readv` | Direct | P1 |
| 20 | writev | `linux_writev` | `_user_writev` | Direct | P1 |
| 257 | openat | `linux_openat` | `_user_open` | Translate (`O_*`, overlay) | P1 |
| 5 | fstat | `linux_newfstat` | `_user_read_stat` | Translate (`struct stat`) | P1 |
| 262 | newfstatat | `linux_newfstatat` | `_user_read_stat` | Translate (`AT_*`, `AT_EMPTY_PATH`) | P1 |
| 217 | getdents64 | `linux_getdents64` | `_user_read_dir` | Translate (`dirent64`, `d_type`) | P1 |
| 9 | mmap | `linux_mmap2` | `_user_map_file`, `vm_create_anonymous_area` | Translate (`MAP_*`) | P1 |
| 11 | munmap | native | `_user_unmap_memory` | Direct | P1 |
| 10 | mprotect | `linux_mprotect` | `_user_set_memory_protection` | Translate | P1 |
| 12 | brk | `linux_brk` | none (libroot emulates it) | New (module) | P1 |
| 158 | arch\_prctl | `linux_arch_prctl` | `user_local_storage`, `x86_set_tls_context` | New, K | P1 |
| 218 | set\_tid\_address | `linux_set_tid_address` | per-thread state | New (module) | P1 |
| 273 | set\_robust\_list | `linux_set_robust_list` | per-thread state | New (module) | P1 |
| 334 | rseq | `linux_rseq` | none | Return `-ENOSYS` | P1 |
| 302 | prlimit64 | `linux_prlimit64` | `_user_getrlimit`, `_user_setrlimit` | Translate (`RLIMIT_*`) | P1 |
| 63 | uname | `linux_newuname` | `_user_get_system_info` | Translate (report `Linux`) | P1 |
| 318 | getrandom | `linux_getrandom` | random bus manager (libroot reaches it via `_kern_generic_syscall(RANDOM_SYSCALLS, RANDOM_GET_ENTROPY)`) | Translate | P1 |
| 228 | clock\_gettime | `linux_clock_gettime` | `_user_get_clock` | Translate (clock IDs) | P1 |
| 35 | nanosleep | `linux_nanosleep` | `_user_snooze_etc` | Translate (remaining time) | P1 |
| 39 | getpid | `linux_getpid` | team ID | Direct | P1 |
| 186 | gettid | `linux_gettid` | thread ID | Direct | P1 |
| 110 | getppid | `linux_getppid` | `team->parent->id` | Direct | P1 |
| 102 / 104 | getuid / getgid | `linux_getuid` / `linux_getgid` | `_user_getresuid`, `_user_getresgid` | Direct | P1 |
| 105 / 106 | setuid / setgid | native | `_user_setresuid`, `_user_setresgid` | Translate | P2 |
| 79 | getcwd | `linux_getcwd` | `_user_getcwd` | Translate (overlay prefix, return length) | P1 |
| 267 | readlinkat | `linux_readlinkat` | `_user_read_link` | Translate (no NUL, `/proc/self/exe`) | P1 |
| 72 | fcntl | `linux_fcntl` | `_user_fcntl` | Translate (`F_*`, locks) | P1 |
| 16 | ioctl | `linux_ioctl` | `_user_ioctl` | Translate per request; `TCGETS`, `TIOCGWINSZ` first | P1 |
| 293 | pipe2 | `linux_pipe2` | `_user_create_pipe` + `fcntl` | Translate | P1 |
| 292 | dup3 | `linux_dup3` | `_user_dup2` + `fcntl` | Translate | P1 |
| 13 | rt\_sigaction | `linux_rt_sigaction` | `_user_sigaction` | Translate (numbers, `SA_RESTORER`) | P1 |
| 14 | rt\_sigprocmask | `linux_rt_sigprocmask` | `_user_set_signal_mask` | Translate (`sigset`) | P1 |
| 15 | rt\_sigreturn | `linux_rt_sigreturn` | `_user_restore_signal_frame` (model) | New (Linux frame), K | P1 |
| 57 | fork | `linux_fork` | `fork_team()` | Translate | P1 |
| 61 | wait4 | `linux_wait4` | `wait_for_child()` | Translate (status, rusage) | P1 |
| 59 | execve | `linux_execve` | `_user_exec` | Translate (personality switch) | P1 |
| 62 | kill | `linux_kill` | `_user_send_signal` | Translate | P1 |
| 7 | poll | `linux_poll` | `_user_poll` | Translate (event bits) | P1 |
| 56 | clone | `linux_clone` | `thread_create_thread` + `forkArgs`, `fork_team` | New, K | P2 |
| 435 | clone3 | `linux_clone3` | as clone | New | P2 |
| 58 | vfork | `linux_vfork` | `fork_team` + parent wait | Translate | P2 |
| 322 | execveat | `linux_execveat` | `_user_exec` | Translate | P2 |
| 247 | waitid | `linux_waitid` | `wait_for_child()` | Translate | P2 |
| 202 | futex | `linux_sys_futex` | none (`user_mutex.cpp` patterns) | New (module) | P2 (P1 with threads) |
| 274 | get\_robust\_list | `linux_get_robust_list` | per-thread state | New | P3 |
| 24 | sched\_yield | native | `_user_thread_yield` | Direct | P2 |
| 234 | tgkill | `linux_tgkill` | `send_signal_to_thread_id()` | Translate | P2 |
| 130 | rt\_sigsuspend | `linux_rt_sigsuspend` | `_user_sigsuspend` | Translate | P2 |
| 131 | sigaltstack | `linux_sigaltstack` | `_user_set_signal_stack` | Translate | P2 |
| 25 | mremap | `linux_mremap` | `vm_resize_area` | New (shrink first) | P2 |
| 28 | madvise | `linux_madvise` | `_user_memory_advice` | Translate (`DONTNEED` zeroing) | P2 |
| 26 | msync | `linux_msync` | `_user_sync_memory` | Translate | P2 |
| 149 / 150 | mlock / munlock | native | `_user_mlock`, `_user_munlock` | Direct | P3 |
| 271 | ppoll | `linux_ppoll` | `_user_poll` + mask | Translate | P2 |
| 291 | epoll\_create1 | `linux_epoll_create1` | `_user_event_queue_create` | Translate | P2 |
| 233 | epoll\_ctl | `linux_epoll_ctl` | `_user_event_queue_select` | Translate (`EPOLLET`, `EPOLLONESHOT`) | P2 |
| 232 / 281 | epoll\_wait / epoll\_pwait | `linux_epoll_wait`, `linux_epoll_pwait` | `_user_event_queue_wait` | Translate | P2 |
| 290 | eventfd2 | `linux_eventfd2` | none | New fd type (module) | P2 |
| 283 | timerfd\_create | `linux_timerfd_create` | none (`UserTimer.cpp` as basis) | New fd type (module) | P3 |
| 294 | inotify\_init1 | `linux_inotify_init1` | node monitor (`node_monitor.cpp`) | New | P3 |
| 41 / 53 | socket / socketpair | `linux_socket`, `linux_socketpair` | `_user_socket`, `_user_socketpair` | Translate (`AF_*`, `SOCK_*`) | P2 |
| 49 / 50 / 42 | bind / listen / connect | `linux_bind`, `linux_listen`, `linux_connect` | `_user_bind`, `_user_listen`, `_user_connect` | Translate (sockaddr) | P2 |
| 288 | accept4 | `linux_accept4` | `_user_accept` + `fcntl` | Translate | P2 |
| 44 / 45 | sendto / recvfrom | `linux_sendto`, `linux_recvfrom` | `_user_sendto`, `_user_recvfrom` | Translate (`MSG_*`) | P2 |
| 46 / 47 | sendmsg / recvmsg | `linux_sendmsg`, `linux_recvmsg` | `_user_sendmsg`, `_user_recvmsg` | Translate (`cmsg`, `SCM_RIGHTS`) | P2 |
| 54 / 55 | setsockopt / getsockopt | `linux_setsockopt`, `linux_getsockopt` | `_user_setsockopt`, `_user_getsockopt` | Translate (levels, options) | P2 |
| 48 | shutdown | `linux_shutdown` | `_user_shutdown_socket` | Direct | P2 |
| 316 | renameat2 | `linux_renameat2` | `_user_rename` | Translate; `RENAME_NOREPLACE`/`EXCHANGE` may need **\[U\]** VFS support | P2 |

Every handler gets three tests before it is marked done: one success case, one expected failure (bad fd, bad pointer giving `-EFAULT`, bad flags giving `-EINVAL`) and one edge case (zero length, `AT_FDCWD`, signal interruption).

## 13. Proposed Haiku source tree

Put the Linux personality in one kernel add-on and keep the hooks in Haiku's kernel small and ABI-neutral ("foreign personality", not "Linux"), so other personalities could use them later. This follows Haiku's existing layout: `src/add-ons/kernel/generic/` already holds non-driver kernel modules such as `tty` and `dpc`. **\[V for the layout, H for the placement\]**

```
headers/private/kernel/
    personality.h                 hook table: probe, load, syscall, signal frame,
                                  thread start/exit, team exec/exit  (new)
src/system/kernel/
    personality.cpp               registration, per-team/thread pointers (new)
    team.cpp                      probe hook in team_create_thread_start_internal()
    thread.cpp                    skip Haiku TLS slots for foreign threads
    signal.cpp                    foreign signal-frame hook
    fs/vfs.cpp                    alternate-root lookup (Stage 5)
    arch/x86/64/interrupts.S      THREAD_FLAGS_FOREIGN_ABI branch
    arch/x86/64/thread.cpp        foreign entry state (rsp = argc, FS = 0)
src/add-ons/kernel/generic/linux/
    Jamfile
    linux_module.cpp              module_info, registers the personality
    abi/                          Linux constants and structs (BSD-2/3 derived)
        linux_errno.h  linux_signal.h  linux_fcntl.h  linux_mman.h
        linux_sched.h  linux_socket.h  linux_stat.h  linux_futex.h (from UAPI)
    syscalls/
        syscalls.master           copied from FreeBSD amd64/linux, trimmed
        gen_sysent.py             generates linux_sysent.cpp
    common/                       architecture-independent handlers
        exec.cpp  errno.cpp  file.cpp  stat.cpp  mmap.cpp  misc.cpp
        process.cpp  thread.cpp  signal.cpp  futex.cpp  socket.cpp
        event.cpp  time.cpp  uid.cpp  dummy.cpp
    arch/x86_64/
        elf_load.cpp  stack.cpp  sigframe.cpp  arch_prctl.cpp
src/add-ons/kernel/file_systems/linprocfs/     (Stage 6)
src/bin/linux/                    'linux' launcher for static musl opt-in
src/tests/system/kernel/linux/
    Jamfile                       runs each binary, compares exit code and output
    src/                          C test sources, built with a Linux cross compiler
    bin/                          prebuilt static x86-64 test binaries
```

Notes:

- No `src/system/libroot/compat/linux/`: a Linux team never maps libroot (section 11).
- Keeping `abi/` headers separate keeps the BSD notices on derived files and makes the license boundary visible.
- Test binaries are built off-Haiku (`x86_64-linux-musl-gcc -static`, later Debian's gcc for glibc) and committed or fetched, so the Haiku build does not need a Linux toolchain. **\[H\]** whether Haiku accepts prebuilt binaries in the tree.

## 14. Technical risk assessment

The two risks that could stop the project are upstream acceptance of the kernel hooks and the signal-range change; the rest are engineering work with known solutions. Ordered by how early they bite:

| Risk | Stage | Impact | Likelihood | Mitigation |
| --- | --- | --- | --- | --- |
| Syscall return path clears `xmm0`–`xmm15` (`CLEAR_FPU_STATE`), silently corrupting Linux code that issues inline `syscall` | 3 | High: random wrong results, not crashes | Certain without a fix | Separate Linux return branch that keeps vector state; test with a binary that checks `xmm` values across `write` |
| `thread.cpp` writes Haiku TLS slots at FS base; fork path asserts on them | 4 | High: glibc TCB corruption | Certain without a fix | Foreign-thread flag skips the block; covered by the PoC's `arch_prctl` test |
| `.Lsyscall_return` compares `r14` to `SYSCALL_RESTORE_SIGNAL_FRAME` by Haiku number | 4 | Medium: wrong return path for one Linux call number | Certain without a fix | Linux path never reaches that compare |
| Haiku has 40 signals; glibc uses 32 and 33 internally | 6 | High: `pthread_cancel`, threaded `setuid` broken | Certain | Discuss widening `MAX_SIGNAL_NUMBER` with Haiku developers early; fall back to a Linux-only extension |
| Process-level `CLONE_VM`/`CLONE_FILES` without `CLONE_THREAD` | 6+ | Medium: some runtimes and sandboxes fail | Low use in common apps | Return `-EINVAL`; implement vfork specially; revisit if real apps need it |
| Kernel cannot easily call `_user_*` with kernel buffers | 4 | Medium: slower progress, duplicated code | Likely for some calls | Per-call audit of `vfs.cpp` internals; copy into user scratch memory where necessary |
| Overlay lookup exposes or hides paths unexpectedly | 5 | Medium: security and confusing behavior | Moderate | Port FreeBSD's lookup rules after reading `vfs_lookup.c`; escape tests |
| PI futexes need scheduler priority inheritance | 6 | Low: only PI mutexes | Certain | `-ENOSYS` first |
| `mremap` grow and move | 5 | Low: `realloc` falls back to copy | Certain | Shrink only, as FreeBSD does |
| `/proc` and `/sys` expectations are broad | 6–7 | Medium | Certain | Implement files on demand, driven by test failures |
| Haiku keeps changing the touched files | all | Medium: rebase cost | Certain | Keep hooks small and ABI-neutral |
| Upstream rejects a Linux personality | all | High: permanent fork | Unknown **\[U\]** | Raise the hook design on Haiku's dev list before Stage 4 |
| Graphics, audio, D-Bus, Flatpak | 8 | High for GUI goals | Certain to be hard | Treat as separate projects; X11/Wayland bridging to app\_server is open research |
| Testing depends on a physical or VM Haiku run each cycle | all | Medium: slow feedback | Certain | Automate: one script builds the image, boots QEMU, runs `src/tests/system/kernel/linux/`, collects serial output |

## 15. First proof of concept, testing and open questions

**Goal:** an unmodified, statically linked Linux x86-64 binary that prints `hello` and exits with status 42 runs on Haiku from a Haiku shell, and every native Haiku test still passes. Size estimate: 1,000–1,500 lines. **\[H\]**

**Test binary** (built on Linux with `musl-gcc -static -nostdlib` so it uses only raw syscalls):

```
_start: mov $1,%eax; mov $1,%edi; lea msg(%rip),%rsi; mov $6,%edx; syscall
        mov $231,%eax; mov $42,%edi; syscall
msg:    .ascii "hello\n"
```

This carries no `PT_INTERP`, no GNU note and `EI_OSABI` 0, so it exercises the explicit opt-in path. A second copy with `EI_OSABI` set to 3 (`elfedit --output-osabi=Linux`) tests automatic detection.

**Steps, each testable on its own:**

1. **Personality hooks (kernel).** `headers/private/kernel/personality.h`, `personality.cpp`, a `void* personality` pointer on `Team` and `Thread`, `THREAD_FLAGS_FOREIGN_ABI`. Test: native boot and the existing kernel tests are unchanged.
2. **Exec probe (kernel).** In `team_create_thread_start_internal()`, before `clone_commpage_area()`, call registered probes with the opened file and its header; on a claim, call the personality's `load()` instead of loading `runtime_loader`. Test: with no module loaded, behavior is identical.
3. **Linux module skeleton.** `src/add-ons/kernel/generic/linux/` with `module_info`, registration, the `EI_OSABI`/`PT_INTERP`/note probe from section 6 and the opt-in attribute. Test: probe logs "claimed" for the test binary, "declined" for `/bin/ls`.
4. **Static `ET_EXEC` loader.** Map `PT_LOAD` segments with `vm_map_file()` and bss with `vm_create_anonymous_area()`, build the Linux stack (argc, argv, envp, auxv with `AT_PHDR`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM`), enter user mode with Linux register state and FS base 0. Test: a binary that loops forever shows up as a running team in `ps`.
5. **Syscall branch (kernel, assembly).** `testl $THREAD_FLAGS_FOREIGN_ABI` in `x86_64_syscall_entry`, call the hook with the iframe, return via `iretq` without `CLEAR_FPU_STATE`. Test: unknown numbers return `-ENOSYS` (checked by a binary that exits with the low byte of `rax`).
6. **Three handlers and errno.** `write` via `_user_write`, `exit`, `exit_group` via `_user_exit_team`; `haiku_to_linux_errno()`. Test: the hello binary prints `hello` and `echo $?` shows 42; `write(99, ...)` returns `-EBADF` (9).
7. **Opt-in launcher.** `src/bin/linux/` sets the attribute or passes a flag so static musl binaries are claimed. Test: `linux ./hello`.

**After the PoC** (Stage 4 start): `arch_prctl`, `brk`, `mmap`/`munmap`/`mprotect`, `read`, `openat`, `close`, `fstat`, `uname`, `set_tid_address`, `rt_sigprocmask`, `ioctl(TIOCGWINSZ)` — enough for a static musl `printf` program and then static BusyBox.

**Testing strategy.**

- **Unit level.** One C file per syscall under `src/tests/system/kernel/linux/src/`, each printing `PASS`/`FAIL` lines and exiting non-zero on failure; success, expected-failure and edge cases as listed in section 12.
- **Reference run.** Every test binary is first run on real Linux; its output is the expected output on Haiku.
- **LTP.** The Linux Test Project's `testcases/kernel/syscalls/` directories are organized per syscall (for example `write/`, `futex/`, `clone/`), which matches the matrix. Build LTP statically against musl and run chosen subsets once Stage 4 is reached. **\[U\]** I did not check LTP's current build options for static musl.
- **Regression.** Native Haiku tests run on every change to the hook files.
- **Status matrix.** A table in the repository, one row per syscall: status, tests passing, notes; updated by the test runner.

**Open questions for you and for Haiku developers:**

- [ ] Should the hooks be proposed upstream from the start, or developed in a fork first?
- [ ] Widen Haiku's signal range for all teams, or add a Linux-only extension?
- [ ] Where should the Linux root live on disk, and what should the opt-in for static musl binaries look like (launcher, file attribute, or both)?
- [ ] Is a Haiku x86-64 VM with serial console available for automated test runs?

## Sources

- FreeBSD source, commit `c502df9eb193`: [github.com/freebsd/freebsd-src](https://github.com/freebsd/freebsd-src/tree/main/sys/compat/linux)
- Haiku source, commit `dd55696fdbd4`: [github.com/haiku/haiku](https://github.com/haiku/haiku)
- [GSoC 2023 alternate proposal: non-native binary execution](https://discuss.haiku-os.org/t/gsoc-2023-alternate-proposal-non-native-binary-execution/13241)
- [My progress on Haiku compatibility layer for Linux (HyClone)](https://discuss.haiku-os.org/t/my-progress-on-haiku-compatibility-layer-for-linux/13346)
- [Linux Test Project](https://github.com/linux-test-project/ltp)
