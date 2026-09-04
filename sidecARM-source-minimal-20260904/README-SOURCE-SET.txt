sidecARM minimal source set
Date: 2026-09-04
Emu68 baseline: 9b4379a
Linux service carveout: 128 MiB

Purpose
-------
Only the current source/configuration material needed to reconstruct the validated sidecARM architecture is included.
Legacy generations, probes, diagnostic POCs, pre-RAW/pre-VBlank copies, relay/PTTY code, 256 MiB DTs, binaries, DTBs, CPIO images, BusyBox binaries, third-party Fitz archive, and historical milestone documents are intentionally excluded.

Canonical AmigaOS sources
-------------------------
amigaos/Linux.c              = current VBlank-safe ARMFB launcher
amigaos/ArmTerm.c            = current RAW/silent VBlank-safe ArmTerm with -fb
amigaos/armnet.device.c      = AmigaOS SANA-II ARMNET driver
amigaos/armnet_shm.h
amigaos/Makefile.armnet

Emu68 patch set
---------------
Apply these files to Emu68 baseline commit 9b4379a at their corresponding original locations:
CMakeLists.txt, M68k_LINE4.c, armservice.c, emu68rom.c, mmu.c, start.c

Linux kernel side
-----------------
armblk-kernel/   shared-memory block device
armnet-kernel/   shared-memory network device
armterm-kernel/  direct /dev/armterm TTY driver
armfb-kernel/    800x600 RGB565 framebuffer with WC mapping and mmap support
sidecarm-128M.dts current 128 MiB DT source with all four devices
kernel-light-valid.config current preferred validated kernel config
kernel-patches/ physical-core1 IRQ fix and FUSE helper

Boot initramfs
--------------
initramfs-armblk-minimal/ contains only the source script and /init needed to build the classic minimal initramfs that mounts /dev/armblk0 and switch_root's into Alpine.
A static AArch64 BusyBox executable must be supplied separately when building it.

Current rootfs integration
--------------------------
The active architecture uses /dev/armterm directly; armterm-relay/PTTY is obsolete and intentionally absent.
Validated /etc/inittab:
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/openrc shutdown
armterm::respawn:/bin/sh -c 'exec /bin/sh -i </dev/armterm >/dev/armterm 2>&1'
tty1::respawn:/sbin/getty 38400 tty1

Typical AmigaOS builds
----------------------
m68k-amigaos-gcc -m68020 -Os -Wall -Wextra -fomit-frame-pointer -noixemul amigaos/ArmTerm.c -o ArmTerm
m68k-amigaos-gcc -m68020 -Os -Wall -Wextra -fomit-frame-pointer -noixemul amigaos/Linux.c -o Linux
