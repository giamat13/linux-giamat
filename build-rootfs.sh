#!/bin/bash
# Builds the Debian root filesystem that carries Xorg + Firefox, and squashes it.
# This is the heavy half of the image: the initramfs only exists to find and
# mount what this script produces. Needs root (debootstrap, chroot, mknod).
set -e
ROOTFS=~/build/rootfs
SUITE=trixie
MIRROR=http://deb.debian.org/debian

if [ "$(id -u)" != 0 ]; then
	echo "run me as root (debootstrap + chroot)" >&2
	exit 1
fi

if [ ! -e "$ROOTFS/etc/debian_version" ]; then
	echo "=== bootstrapping Debian $SUITE ==="
	rm -rf "$ROOTFS"
	debootstrap --variant=minbase --arch=amd64 "$SUITE" "$ROOTFS" "$MIRROR"
fi

# Not -l: a lazy unmount returns before the mount is really gone, and mksquashfs
# then races it and squashes a live /proc into a broken image.
cleanup() { umount "$ROOTFS"/{proc,sys,dev} 2>/dev/null || true; }
trap cleanup EXIT
mount --bind /proc "$ROOTFS/proc"
mount --bind /sys  "$ROOTFS/sys"
mount --bind /dev  "$ROOTFS/dev"

echo "=== installing Xvfb + Firefox ==="
# No Xorg: Firefox runs on a headless Xvfb and fbdesktop copies its screen into
# a window. That means no VT juggling, no DRM master, no udev-fed input devices.
chroot "$ROOTFS" /bin/bash -c '
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	# xkb-data + xkbcomp are not optional: Xvfb refuses to start without a
	# keymap it can compile, even though nothing here has a real keyboard.
	apt-get install -y --no-install-recommends \
		xvfb xkb-data x11-xkb-utils firefox-esr fonts-dejavu-core \
		ca-certificates dbus-x11 libgl1-mesa-dri libx11-6 libxext6 libxtst6
	apt-get clean
'

echo "=== fbdesktop + its runtime ==="
install -m 755 ~/build/fbdesktop "$ROOTFS/bin/fbdesktop"
# The Debian base has no DHCP client; busybox brings udhcpc along with it.
install -m 755 /bin/busybox "$ROOTFS/bin/busybox"

mkdir -p "$ROOTFS/usr/share/udhcpc"
cat > "$ROOTFS/usr/share/udhcpc/default.script" <<'EOF'
#!/bin/busybox sh
[ -z "$1" ] && exit 1
case "$1" in
deconfig)
	busybox ifconfig "$interface" 0.0.0.0
	;;
renew|bound)
	busybox ifconfig "$interface" "$ip" netmask "${subnet:-255.255.255.0}"
	[ -n "$router" ] && busybox route add default gw "${router%% *}" dev "$interface"
	: > /etc/resolv.conf
	for d in $dns; do
		echo "nameserver $d" >> /etc/resolv.conf
	done
	;;
esac
exit 0
EOF
chmod 755 "$ROOTFS/usr/share/udhcpc/default.script"

# PID 1 after switch_root. Deliberately not systemd -- there is one job here.
cat > "$ROOTFS/sbin/init" <<'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null
mkdir -p /dev/pts && mount -t devpts none /dev/pts
mount -t tmpfs none /tmp
mkdir -p /run
mount -t tmpfs none /run
# Without a hostname the machine is literally "(none)", and xauth then builds the
# display name "(none):0". It also has to resolve, or xauth rejects it and X
# comes up with no cookie for Firefox to authenticate against.
hostname linux-giamat
echo "127.0.0.1 localhost linux-giamat" > /etc/hosts
# Xvfb needs this to exist and be world-writable, or it cannot bind its socket.
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix

export HOME=/root
# A shell on the serial line: the framebuffer belongs to fbdesktop, so this is
# the only way to inspect a running system (and it costs nothing on real HW).
setsid /bin/sh < /dev/ttyS0 > /dev/ttyS0 2>&1 &
echo "=================================================="
echo " Custom Linux fork - built from linux-giamat source"
echo "=================================================="
uname -a

: > /etc/resolv.conf
busybox ifconfig lo 127.0.0.1 up
for dev in /sys/class/net/*; do
	nic=$(basename "$dev")
	[ "$nic" = "lo" ] && continue
	echo "Configuring $nic via DHCP..."
	busybox ifconfig "$nic" up
	# -s is not optional: busybox's built-in script path does not exist here.
	busybox udhcpc -i "$nic" -s /usr/share/udhcpc/default.script -t 5 -n -q && break
done

echo "Starting graphical desktop..."
/bin/fbdesktop
echo "fbdesktop exited, dropping to shell"
exec /bin/sh
EOF
chmod 755 "$ROOTFS/sbin/init"

cleanup
trap - EXIT

echo "=== squashing ==="
# The bind mounts are gone by now, so these are empty dirs -- and they must stay
# in the image: excluding them removes the mount points /sbin/init needs.
mkdir -p "$ROOTFS"/{proc,sys,dev,tmp,run,mnt}
if mount | grep -q " $ROOTFS/\(proc\|sys\|dev\) "; then
	echo "still mounted under $ROOTFS -- refusing to squash a live /proc" >&2
	exit 1
fi
rm -f ~/build/rootfs.squashfs
mksquashfs "$ROOTFS" ~/build/rootfs.squashfs -comp zstd -noappend
ls -la ~/build/rootfs.squashfs
