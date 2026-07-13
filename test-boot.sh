#!/bin/bash
timeout 20 qemu-system-x86_64 -m 512 -cdrom ~/build/linux-giamat.iso -nographic -serial mon:stdio -display none > /tmp/qemu_out.txt 2>&1
grep -n -i "custom\|Linux version\|init process" /tmp/qemu_out.txt
