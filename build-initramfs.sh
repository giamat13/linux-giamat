#!/bin/bash
set -e
ROOT=~/build/initramfs
rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,etc,proc,sys,dev,tmp,root,lib64}
mkdir -p "$ROOT"/usr/share/terminfo/l "$ROOT"/etc/ssl/certs "$ROOT"/usr/share/udhcpc
cp /bin/busybox "$ROOT/bin/busybox"
cd "$ROOT/bin"
for cmd in sh ls cat mount ps mkdir echo uname clear ln df free dmesg reboot poweroff \
           head tail grep sort uptime wc cut date kill sleep \
           ifconfig route udhcpc hostname ping; do
	ln -sf busybox "$cmd"
done

cp ~/build/fbdesktop "$ROOT/bin/fbdesktop"
chmod +x "$ROOT/bin/fbdesktop"

# busybox is static, but lynx is not -- it is the first dynamically linked binary
# in this image, so it needs its shared libraries and the ELF loader copied in.
copy_deps() {
	ldd "$1" 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^\//) { print $i; break }}' |
	while read -r lib; do
		dest="$ROOT$(dirname "$lib")"
		mkdir -p "$dest"
		cp -Ln "$lib" "$dest/" 2>/dev/null || true
	done
}

cp "$(which lynx)" "$ROOT/bin/lynx"
copy_deps "$ROOT/bin/lynx"

# ncurses needs the terminfo entry for the TERM fbdesktop hands its children,
# and gnutls needs a trust store or every https:// fetch fails.
cp /usr/share/terminfo/l/linux "$ROOT/usr/share/terminfo/l/linux"
cp /etc/ssl/certs/ca-certificates.crt "$ROOT/etc/ssl/certs/"
[ -d /etc/lynx ] && cp -r /etc/lynx "$ROOT/etc/"

cat > "$ROOT/usr/share/udhcpc/default.script" <<'EOF'
#!/bin/busybox sh
[ -z "$1" ] && exit 1
case "$1" in
deconfig)
	ifconfig "$interface" 0.0.0.0
	;;
renew|bound)
	ifconfig "$interface" "$ip" netmask "${subnet:-255.255.255.0}"
	[ -n "$router" ] && route add default gw "${router%% *}" dev "$interface"
	: > /etc/resolv.conf
	for d in $dns; do
		echo "nameserver $d" >> /etc/resolv.conf
	done
	;;
esac
exit 0
EOF
chmod +x "$ROOT/usr/share/udhcpc/default.script"

cat > "$ROOT/init" <<'EOF'
#!/bin/busybox sh
/bin/busybox mount -t proc none /proc
/bin/busybox mount -t sysfs none /sys
/bin/busybox mount -t devtmpfs none /dev
/bin/busybox mkdir -p /dev/pts
/bin/busybox mount -t devpts none /dev/pts
export HOME=/root
export TERMINFO=/usr/share/terminfo
echo "=================================================="
echo " Custom Linux fork - built from linux-giamat source"
echo "=================================================="
uname -a
echo

# Bring up networking, or the browser has nothing to browse. The NIC name is
# whatever the kernel probed (e1000 -> eth0), so ask sysfs rather than guess.
: > /etc/resolv.conf
ifconfig lo 127.0.0.1 up
for dev in /sys/class/net/*; do
	nic=$(basename "$dev")
	[ "$nic" = "lo" ] && continue
	echo "Configuring $nic via DHCP..."
	ifconfig "$nic" up
	# -s is not optional: busybox's built-in script path does not exist here, so
	# without it the lease is obtained and then silently thrown away.
	udhcpc -i "$nic" -s /usr/share/udhcpc/default.script -t 5 -n -q && break
done

echo "Starting graphical desktop..."
/bin/fbdesktop
echo "fbdesktop exited, dropping to shell"
exec /bin/busybox sh
EOF
chmod +x "$ROOT/init"

cd "$ROOT"
find . | cpio -o -H newc | gzip > ~/build/initramfs.img
ls -la ~/build/initramfs.img
