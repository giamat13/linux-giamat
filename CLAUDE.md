# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A fork of the Linux kernel (7.2-rc3) plus a small out-of-tree userspace experiment. Nearly all files are upstream kernel source — treat them as upstream unless `git log` shows a local commit touching them. The local work is only:

- `fbdesktop.c` — a standalone userspace framebuffer desktop (not kernel code, not built by Kbuild).
- `build-initramfs.sh` / `build-iso.sh` — package the kernel + fbdesktop into a bootable ISO.
- Local netfilter changes (`net/netfilter/xt_{hl,dscp,rateest,tcpmss}.c` and their uapi headers, commit `1db51fc`).

## Hard requirement: must boot on real hardware

The ISO is meant to boot on a real, separate PC — QEMU is only a fast smoke test, never the target. Anything that only works because of the dev host (WSL paths, Windows drives, host binaries/libs, `/mnt/c`, network shares back to the host, QEMU-only devices or virtio drivers, a display mode only QEMU offers) is a bug, not a shortcut. Everything the running system needs must be inside the kernel image or the initramfs, statically resolvable at boot with no host in the picture. Assume real hardware: different GPU/framebuffer resolution, USB keyboard and mouse (not PS/2), real disks, and no serial console. Before calling something done, ask "would this still work if I burned the ISO to a USB stick and booted a machine that has never seen WSL?" — if not, fix it.

## Build & boot loop

Builds run on Linux (WSL), not Windows. The scripts assume the tree is checked out at `~/build/linux-giamat` and write artifacts into `~/build/`:

```sh
make -j$(nproc) bzImage          # -> arch/x86/boot/bzImage
cc -O2 -o ~/build/fbdesktop fbdesktop.c
./build-initramfs.sh             # busybox + fbdesktop -> ~/build/initramfs.img
./build-iso.sh                   # grub-mkrescue -> ~/build/linux-giamat.iso
qemu-system-x86_64 -cdrom ~/build/linux-giamat.iso -m 1G -serial stdio
```

`build-initramfs.sh` writes the `/init` script inline (heredoc): it mounts proc/sys/devtmpfs/devpts, runs `/bin/fbdesktop`, and drops to a busybox shell if it exits. Any new runtime dependency (a busybox applet, a device node, an env var) must be added there — there is no other init.

The kernel config must have framebuffer console, evdev/mousedev, and devtmpfs enabled for fbdesktop to find `/dev/fb0`, `/dev/input/*`, and the VT font. GRUB sets `gfxpayload=1024x768x32` in `build-iso.sh`.

## fbdesktop architecture

Single file, no X11, no toolkit. It draws pixels straight into a mmap'd `/dev/fb0`, reads the mouse from `/dev/input/mice` (or evdev), takes keyboard from stdin in raw termios mode, and lifts its bitmap font from the kernel's own VT console font via `KDFONTOP` — so the font depends on the running kernel, not on any file in the initramfs.

The model is a small window manager: icons open windows, windows are draggable/resizable, and there's a taskbar. Two window kinds — a pty-backed VT100-ish terminal rendered onto a character grid, and a one-shot command-output view. State lives in fixed-size arrays (`MAX_WIN`, `GRID_MAXCOLS/ROWS`); there is no allocation-heavy scene graph. Changing layout constants at the top of the file is usually the right knob rather than adding structure.

Because it owns the framebuffer and the tty directly, a crash leaves the console in raw mode — boot the ISO in QEMU to test rather than running it on a host tty.

## Kernel work

Standard upstream conventions apply (`Documentation/process/`, `scripts/checkpatch.pl`). Build a subsystem alone with `make net/netfilter/` or a single object with `make net/netfilter/xt_hl.o`; `make W=1` for the stricter warnings. Kernel code has no test runner here — verification is booting the ISO.
