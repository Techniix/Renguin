# Renguin Manager: plan

A native Haiku application for installing, inspecting and configuring
Renguin. Status: **planned**. Most of it needs process creation
(`fork`/`execve`), so building starts after that lands. Parts marked
**early** can be done before then.

## Goals

- Running Linux software on Haiku should feel native: no Terminal needed to
  set up a Linux userland or to see why a program doesn't run.
- Built with Haiku's own API (BApplication/BWindow/layout API), so it looks
  and behaves like the rest of Haiku.
- No new kernel interfaces unless one is clearly needed. Read what the module
  already exposes (the syslog, the Linux root on disk) first.

## Features, in order

1. **Status (early).** Shows whether the Renguin module is installed and
   loaded, whether a Linux root exists, and what's in it (distribution name
   from `/etc/os-release`, size).
2. **Compatibility log (early).** Live view of `renguin: team N:
   unimplemented Linux syscall X (name)` lines, grouped by program, with
   counts. This is the main tool for deciding which syscalls to implement
   next, and lets users attach a ready-made report to a bug. Source: the
   syslog to start with; a dedicated interface later if needed.
3. **Linux userland installer (early, download and unpack only).** Choose
   Alpine (musl, small) or Debian/Ubuntu (glibc), then download the root
   filesystem and unpack it into `/boot/system/non-packaged/compat/linux`.
   Verify checksums. Support more than one root later.
4. **Tracker integration (early).** A MIME type and icon for Linux ELF
   programs (`application/x-vnd.renguin-linux-executable` or similar), plus a
   sniffer rule, so Linux programs are recognizable in Tracker and open with
   a double-click. Opening already works through `load_image()`.
5. **Package manager front-end (needs fork/exec).** Search, install and
   remove packages with the root's own `apk` or `apt`, with progress
   reporting, and add installed GUI applications to the Deskbar menu from
   their `.desktop` files.
6. **Per-program settings (later).** Environment variables, which Linux root
   to use, and passthrough options such as sharing `/tmp/discord-ipc-*` for
   Discord Rich Presence between Haiku and Linux programs.
7. **Deskbar replicant (later).** Running Linux programs at a glance.

## Open questions

- Should the module expose status through a device file or a syscall
  instead of the syslog? Decide once the syslog approach shows its limits.
- Where to keep per-program settings, so the module can read them at exec
  time (a settings file in `~/config/settings/renguin/`?).
- Ship as its own HaikuPorts package, separate from the module.
