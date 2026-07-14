#!/bin/bash
set -e
ISODIR=~/build/isoroot
rm -rf "$ISODIR"
mkdir -p "$ISODIR/boot/grub"

cp ~/build/linux-giamat/arch/x86/boot/bzImage "$ISODIR/boot/vmlinuz"
cp ~/build/initramfs.img "$ISODIR/boot/initramfs.img"
# The real system rides along as a squashfs; /init mounts it off the CD.
cp ~/build/rootfs.squashfs "$ISODIR/rootfs.squashfs"

cat > "$ISODIR/boot/grub/grub.cfg" <<'EOF'
set timeout=5
set default=0
insmod vbe
insmod video_bochs
insmod video_cirrus
set gfxpayload=1024x768x32

menuentry "linux-giamat fork" {
	linux /boot/vmlinuz console=ttyS0 console=tty0
	initrd /boot/initramfs.img
}
EOF

grub-mkrescue -o ~/build/linux-giamat.iso "$ISODIR" 2>&1 | tail -20
ls -la ~/build/linux-giamat.iso
