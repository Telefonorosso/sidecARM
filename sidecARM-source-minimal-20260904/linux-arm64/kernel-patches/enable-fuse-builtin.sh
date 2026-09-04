#!/bin/sh
set -eu
# Run from the Linux kernel source tree used by the PiStorm ARM64 service.
# The 2026-08-31 Fitz R/W milestone uses FUSE built into the Image so the
# initramfs does not need fuse.ko/module loading infrastructure.
scripts/config --enable FUSE_FS
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
grep CONFIG_FUSE_FS .config
