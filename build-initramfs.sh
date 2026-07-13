#!/bin/bash
set -e
ROOT=~/build/initramfs
rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,etc,proc,sys,dev,tmp,root}
cp /bin/busybox "$ROOT/bin/busybox"
cd "$ROOT/bin"
for cmd in sh ls cat mount ps mkdir echo uname clear ln df free dmesg reboot poweroff; do
	ln -sf busybox "$cmd"
done

cp ~/build/fbdesktop "$ROOT/bin/fbdesktop"
chmod +x "$ROOT/bin/fbdesktop"

cat > "$ROOT/init" <<'EOF'
#!/bin/busybox sh
/bin/busybox mount -t proc none /proc
/bin/busybox mount -t sysfs none /sys
/bin/busybox mount -t devtmpfs none /dev
/bin/busybox mkdir -p /dev/pts
/bin/busybox mount -t devpts none /dev/pts
echo "=================================================="
echo " Custom Linux fork - built from linux-giamat source"
echo "=================================================="
uname -a
echo
echo "Starting graphical desktop..."
/bin/fbdesktop
echo "fbdesktop exited, dropping to shell"
exec /bin/busybox sh
EOF
chmod +x "$ROOT/init"

cd "$ROOT"
find . | cpio -o -H newc | gzip > ~/build/initramfs.img
ls -la ~/build/initramfs.img
