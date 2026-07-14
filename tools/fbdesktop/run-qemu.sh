#!/bin/sh
# Boot the ISO in QEMU, for the dev loop only -- the real target is a physical
# PC booting from USB. Run from WSL:  ./tools/fbdesktop/run-qemu.sh
#
# Every flag below is load-bearing:
#
#   -accel kvm -cpu host   without KVM, QEMU emulates the CPU and the guest
#                          (Firefox on llvmpipe especially) is 10-20x slower.
#   GDK_BACKEND=x11        QEMU's window is GTK. Under WSLg, GTK otherwise picks
#   -display gtk,gl=off    Wayland and the window opens, takes focus, and never
#                          paints a pixel.
#
# And the reason this is a script rather than one long command line: WSLg's
# XWayland reports a 640x480 screen, while our window is ~1280x825. WSLg then
# places the window at some arbitrary position outside that screen (x=1663,
# x=1773, it varies per run). The window is mapped and painted -- it shows up in
# the Windows taskbar -- but it lands off the right edge of the desktop, so you
# see nothing. QEMU's GTK backend has no geometry flag, so we move the window
# ourselves once it exists.

set -e

ISO="${ISO:-$HOME/build/linux-giamat.iso}"
[ -f "$ISO" ] || { echo "no ISO at $ISO -- run build-iso.sh first" >&2; exit 1; }

export DISPLAY="${DISPLAY:-:0}"
export GDK_BACKEND=x11

qemu-system-x86_64 \
	-accel kvm -cpu host -m 3G -smp 4 \
	-cdrom "$ISO" \
	-vga std -display gtk,gl=off \
	-usb -device usb-tablet \
	-netdev user,id=n0 -device e1000,netdev=n0 \
	-serial mon:stdio &
qemu_pid=$!

# Drag the window back onto the visible desktop. Needs x11-utils + xdotool on
# the dev host (apt install x11-utils xdotool); without them QEMU still runs,
# you just may not be able to see it.
if command -v xwininfo >/dev/null && command -v xdotool >/dev/null; then
	i=0
	while [ $i -lt 60 ]; do
		wid=$(xwininfo -root -tree 2>/dev/null | grep '"QEMU":' | head -1 | awk '{print $1}')
		[ -n "$wid" ] && break
		i=$((i + 1))
		sleep 0.25
	done
	# The window is placed by WSLg a moment after it is created, and again when
	# the guest switches out of VGA text mode into its real resolution -- so
	# move it more than once, after the mode switch has settled.
	if [ -n "$wid" ]; then
		for delay in 2 8 20; do
			sleep "$delay"
			xdotool windowmove "$wid" 0 0 2>/dev/null || true
		done
		xdotool windowactivate "$wid" 2>/dev/null || true
	fi
else
	echo "note: xwininfo/xdotool missing -- QEMU's window may open off-screen." >&2
	echo "      apt install x11-utils xdotool" >&2
fi

wait "$qemu_pid"
