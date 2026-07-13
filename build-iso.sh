#!/bin/bash
set -e
ISODIR=~/build/isoroot
rm -rf "$ISODIR"
mkdir -p "$ISODIR/boot/grub"

cp ~/build/linux-giamat/arch/x86/boot/bzImage "$ISODIR/boot/vmlinuz"
cp ~/build/initramfs.img "$ISODIR/boot/initramfs.img"

cat > "$ISODIR/boot/grub/grub.cfg" <<'EOF'
set timeout=5
set default=0

menuentry "linux-giamat fork" {
	linux /boot/vmlinuz console=ttyS0 console=tty0
	initrd /boot/initramfs.img
}
EOF

grub-mkrescue -o ~/build/linux-giamat.iso "$ISODIR" 2>&1 | tail -20
ls -la ~/build/linux-giamat.iso
