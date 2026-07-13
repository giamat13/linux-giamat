#!/bin/bash
set -e
cd ~/build/linux-giamat
scripts/config --enable CONFIG_INPUT_MOUSEDEV
scripts/config --enable CONFIG_INPUT_EVDEV
make olddefconfig
grep -E "CONFIG_INPUT_MOUSEDEV|CONFIG_INPUT_EVDEV" .config
echo "--- building ---"
make -j$(nproc) 2>&1 | tee ~/build/build3.log | tail -20
ls -la arch/x86/boot/bzImage
