#!/bin/bash
set -e
cd ~/build/linux-giamat
scripts/config --enable CONFIG_FB
scripts/config --enable CONFIG_FRAMEBUFFER_CONSOLE
scripts/config --enable CONFIG_SYSFB_SIMPLEFB
scripts/config --enable CONFIG_FB_SIMPLE
scripts/config --enable CONFIG_DRM_SIMPLEDRM
scripts/config --enable CONFIG_DRM_FBDEV_EMULATION
make olddefconfig
grep -E "CONFIG_FB=|CONFIG_FB_SIMPLE|CONFIG_DRM_SIMPLEDRM|CONFIG_SYSFB_SIMPLEFB|CONFIG_FRAMEBUFFER_CONSOLE|CONFIG_DRM_FBDEV_EMULATION" .config
echo "--- building ---"
make -j$(nproc) 2>&1 | tee ~/build/build2.log | tail -30
echo "--- build done, checking bzImage timestamp ---"
ls -la arch/x86/boot/bzImage
