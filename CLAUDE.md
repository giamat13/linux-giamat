# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A fork of the Linux kernel (7.2-rc3) plus a small out-of-tree userspace experiment. Nearly all files are upstream kernel source — treat them as upstream unless `git log` shows a local commit touching them. The local work so far has been only:

- `tools/fbdesktop/` — a standalone userspace framebuffer desktop (the shell: framebuffer, windows, character grid, event loop) plus its `Makefile` and `include/`, and `tools/apps/` — one file per application window. Built out-of-tree with `make -C tools/fbdesktop`, which links both into the single `fbdesktop` binary. Userspace, not kernel code, never built by Kbuild — it lives under `tools/` because that is where this tree keeps its userspace programs (`tools/perf`, `tools/bpf`, …).
- `build-rootfs.sh` / `build-initramfs.sh` / `build-iso.sh` — package the kernel + a Debian rootfs + fbdesktop into a bootable ISO.
- Local netfilter changes (`net/netfilter/xt_{hl,dscp,rateest,tcpmss}.c` and their uapi headers, commit `1db51fc`).

This list describes where past changes happened to land, not a restriction on where future ones may land. Pick whichever files the task actually needs — including kernel subsystems, Kconfig, or drivers — rather than defaulting to the userspace desktop out of habit.

**Priority order when choosing an approach and which files to touch:**
1. Correctness and quality first — the result must actually be the best solution to the task (and must still satisfy "must boot on real hardware" below). Never downgrade quality to save cost.
2. Among approaches that are equally good on (1), pick the one that costs the fewest credits: the smallest diff, fewest files touched, fewest tool calls/exploration needed, least back-and-forth. If several options tie on quality, take the cheapest one, not just a cheaper one.

## Hard requirement: must boot on real hardware

The ISO is meant to boot on a real, separate PC — QEMU is only a fast smoke test, never the target. Anything that only works because of the dev host (WSL paths, Windows drives, host binaries/libs, `/mnt/c`, network shares back to the host, QEMU-only devices or virtio drivers, a display mode only QEMU offers) is a bug, not a shortcut. Everything the running system needs must be inside the kernel image or the initramfs, statically resolvable at boot with no host in the picture. Assume real hardware: different GPU/framebuffer resolution, USB keyboard and mouse (not PS/2), real disks, and no serial console. Before calling something done, ask "would this still work if I burned the ISO to a USB stick and booted a machine that has never seen WSL?" — if not, fix it.

## Build & boot loop

Builds run on Linux (WSL), not Windows. The kernel tree lives at `~/build/linux-giamat` (a separate copy from the git checkout, which is reachable from WSL at `/mnt/c/Users/<you>/code/linux-giamat`); artifacts go to `~/build/`.

Before running `qemu-system-x86_64`, make sure it can't crash or destabilize the real (host) machine it runs on — check available RAM/CPU headroom before picking `-m`/`-smp`, and don't enable device/passthrough options that touch host hardware directly.

```sh
make -j$(nproc) bzImage                              # -> arch/x86/boot/bzImage
make -C tools/fbdesktop -j$(nproc)                   # -> ~/build/fbdesktop
sudo ./build-rootfs.sh   # debootstrap + Xvfb + Firefox -> ~/build/rootfs.squashfs
./build-initramfs.sh     # tiny busybox initramfs       -> ~/build/initramfs.img
./build-iso.sh           # grub-mkrescue                -> ~/build/linux-giamat.iso
```

`build-rootfs.sh` needs root and is the slow step (~1.2 GB rootfs, squashed to ~290 MB; the ISO is ~320 MB). It caches the debootstrap: it only re-bootstraps if `$ROOTFS/etc/debian_version` is missing.

**Boot it exactly like `.vscode/launch.json` does.** Two flags there are not decoration:
- `-accel kvm -cpu host` — without KVM, QEMU emulates the CPU and everything (especially Firefox on llvmpipe) is 10-20x slower. `/dev/kvm` is available under WSL2.
- `DISPLAY=:0 GDK_BACKEND=x11 ... -display gtk,gl=off` — **under WSLg, GTK defaults to Wayland and QEMU's window opens, takes focus, and never paints a pixel.** This looks exactly like the guest hanging and it is not. If you need to verify a screenshot programmatically, `-vnc :1` and drive it over RFB instead; QEMU's HMP `mouse_move` does not work with `usb-tablet`, so use real RFB pointer events.

## Boot architecture

Not an initramfs-only system any more. The initramfs is tiny and does one job: find `/dev/sr0`, loop-mount `rootfs.squashfs` off the CD, stack a tmpfs over it with overlayfs (so the read-only image looks writable), and `switch_root` into it. Everything else — glibc, Xvfb, Firefox, fbdesktop — lives in the squashfs.

`/sbin/init` in the rootfs (written inline by `build-rootfs.sh`) is a plain shell script, not systemd: it mounts the pseudo-filesystems, sets a hostname (xauth rejects a display name built from `(none)`), brings up DHCP with busybox `udhcpc`, and execs `/bin/fbdesktop`. Any new runtime dependency goes there — there is no other init.

`udhcpc` must be passed `-s /usr/share/udhcpc/default.script` explicitly. Its built-in script path does not exist here, and without `-s` it obtains a lease and then silently discards it — you get an interface with no IPv4 and an empty `/etc/resolv.conf`.

The kernel config needs framebuffer console, evdev/mousedev, devtmpfs, `SQUASHFS`, `OVERLAY_FS`, `BLK_DEV_LOOP`, `ISO9660_FS`, `BLK_DEV_SR`, `SYSVIPC` (the browser's shared-memory capture), and a KMS driver (`DRM_BOCHS` for QEMU, `DRM_I915` etc. for real hardware).

## fbdesktop architecture

A small multi-file program, no toolkit. It is split the way `tools/perf` is: one shell plus the things it launches, all linked into one binary. The shell lives in `tools/fbdesktop/`; each application window is one file in `tools/apps/`. They are not separate programs — an "app" here is a window kind, and it links into the same executable. Shared types, constants, extern globals and cross-module prototypes live in `tools/fbdesktop/include/fbdesktop.h`; everything else is `static` inside its own file.

| `tools/fbdesktop/` (the shell) | Responsibility |
| --- | --- |
| `main.c` | framebuffer/font/input setup, hit testing, icon launching, event loop |
| `desktop.c` | file-type classification, icons, desktop directory, taskbar, context menu, compositor (`redraw_all`) |
| `window.c` | window records: z-order, focus, open/close/maximize/clamp, frame drawing |
| `draw.c` | framebuffer primitives: pixels, rects, text, vector glyphs |
| `grid.c` | the character grid + VT100-ish parser every window type renders into |
| `globals.c` | definitions of the shared state declared `extern` in the header |

| `tools/apps/` (one window kind each) | Responsibility |
| --- | --- |
| `terminal.c` | live pty-backed terminal, and the one-shot command-output view |
| `files.c` | file manager: listing, search, clipboard, drag-and-drop, file operations |
| `editor.c` | text editor |
| `taskmgr.c` | task manager tabs + CPU/memory sampler and graphs |
| `settings.c` | settings |
| `browser.c` | Xvfb + Firefox, MIT-SHM capture, XTEST input |

A new app is a new file in `tools/apps/`, its name added to `APP_SRCS` in the Makefile, an icon in `icons[]` (`globals.c`), and a case in `launch_icon()` (`main.c`).

It draws pixels straight into a mmap'd `/dev/fb0`, reads the mouse from an evdev absolute pointer (or `/dev/input/mice`), takes keyboard from stdin in raw termios mode, and lifts its bitmap font from the kernel's own VT console font via `KDFONTOP` — so the font depends on the running kernel, not on any file in the image.

The model is a small window manager: icons open windows, windows are draggable/resizable/minimisable, and there's a taskbar. State lives in fixed-size arrays (`MAX_WIN`, `GRID_MAXCOLS/ROWS`); there is no allocation-heavy scene graph. Changing layout constants in `tools/fbdesktop/include/fbdesktop.h` is usually the right knob rather than adding structure.

**The browser window (`WIN_BROWSER`) is the one thing fbdesktop does not draw itself.** Firefox runs on a headless Xvfb sized to the whole screen; fbdesktop is an X client of it, pulls the root image over MIT-SHM each frame, blits it into an ordinary window, and feeds clicks and keys back with XTEST. That is why it minimises, maximises and sits in the taskbar like any other window. Firefox's top-level is resized to the window's content area on every geometry change (override-redirect children are skipped — they are menus and popups, and stretching them wrecks them). There is no Xorg, no VT switching and no DRM master anywhere in this path.

Two traps that cost real time here:
- **`SIG_IGN` on `SIGCHLD` survives `exec`.** fbdesktop ignores SIGCHLD; a child that inherits that and then waits on its own children gets `ECHILD`. Xvfb forks `xkbcomp`, so it dies with a misleading "failed to compile keymap". Every child forked from fbdesktop must `signal(SIGCHLD, SIG_DFL)` before `exec`.
- **Do not put a VT into `KD_GRAPHICS` and then expect X to start on another one.** The kernel refuses to switch away from a graphics-mode VT, and X hangs forever inside `VT_WAITACTIVE` with no error. (Moot now that the browser uses Xvfb, but it is the reason it does.)

Because it owns the framebuffer and the tty directly, a crash leaves the console in raw mode — boot the ISO in QEMU to test rather than running it on a host tty. `/sbin/init` also starts a shell on `/dev/ttyS0`: when the framebuffer is owned by fbdesktop, that serial shell is the only way to inspect a running system, and it is where Xvfb and Firefox errors are sent.

## Workflow

Always maintain a TODO list (via the TodoWrite tool) for any non-trivial task in this repo, and keep it updated as work progresses.

Delegate to subagents. Hand off most small, self-contained tasks; keep the large and architectural work yourself. Always verify the whole result yourself at the end — a subagent's report is a claim, never verification.

Put every file in the directory where it belongs: an existing one if one fits, a new one if none does. The repo root is a kernel tree — never dump loose source files there. Userspace code goes under `tools/`, in a directory named for what it is, following whatever the surrounding tree already does. When unsure how to lay something out, look at how this repo (or a well-known project) already solves the same shape, rather than inventing a structure.

## Kernel work

Standard upstream conventions apply (`Documentation/process/`, `scripts/checkpatch.pl`). Build a subsystem alone with `make net/netfilter/` or a single object with `make net/netfilter/xt_hl.o`; `make W=1` for the stricter warnings. Kernel code has no test runner here — verification is booting the ISO.
