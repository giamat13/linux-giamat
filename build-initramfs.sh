#!/bin/bash
# The initramfs no longer *is* the system -- it only has to find the CD, stack a
# writable tmpfs over the read-only squashfs on it, and switch into that.
set -e
ROOT=~/build/initramfs
rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,proc,sys,dev,mnt/cdrom,mnt/lower,mnt/rw,newroot}
cp /bin/busybox "$ROOT/bin/busybox"
cd "$ROOT/bin"
for cmd in sh mount umount mkdir switch_root sleep echo cat ls losetup; do
	ln -sf busybox "$cmd"
done

cat > "$ROOT/init" <<'EOF'
#!/bin/busybox sh
/bin/busybox mount -t proc none /proc
/bin/busybox mount -t sysfs none /sys
/bin/busybox mount -t devtmpfs none /dev

# The CD may take a moment to show up after the SCSI probe.
for i in 1 2 3 4 5 6 7 8 9 10; do
	[ -b /dev/sr0 ] && break
	/bin/busybox sleep 1
done

mount -t iso9660 -o ro /dev/sr0 /mnt/cdrom || { echo "no CD found"; exec /bin/busybox sh; }
mount -t squashfs -o ro,loop /mnt/cdrom/rootfs.squashfs /mnt/lower || { echo "no squashfs"; exec /bin/busybox sh; }

# squashfs is read-only, but Firefox wants a profile and X wants sockets, so
# put a RAM layer on top and let the whole root look writable.
mount -t tmpfs none /mnt/rw
mkdir -p /mnt/rw/upper /mnt/rw/work
mount -t overlay overlay \
	-o lowerdir=/mnt/lower,upperdir=/mnt/rw/upper,workdir=/mnt/rw/work /newroot \
	|| { echo "overlay failed"; exec /bin/busybox sh; }

mount --move /mnt/cdrom /newroot/mnt 2>/dev/null
exec switch_root /newroot /sbin/init
EOF
chmod +x "$ROOT/init"

cd "$ROOT"
find . | cpio -o -H newc | gzip > ~/build/initramfs.img
ls -la ~/build/initramfs.img
