# sidecARM Source Overview

## AmigaOS

### `amigaos/src/Linux.c`

Main AmigaOS launcher for sidecARM. It loads the Linux kernel, DTB and initramfs into the reserved ARM service memory, starts the AmigaOS-side `armblk` backend, patches the device tree at runtime, and finally hands physical CPU1 over to Linux.

This is also where the current **128 MB service carveout** is enforced, together with the runtime DT fixes for `armblk`, `armnet` and `armterm`. For the current milestone, this is one of the most critical files and should not be replaced by older launcher variants.

### `amigaos/src/ArmTerm-v8.c`

AmigaOS terminal client for the Linux service. It talks to the ArmTerm shared-memory interface and provides an interactive Linux console directly from AmigaOS.

ArmTerm is especially useful before networking is fully operational, since it gives immediate access to the Linux shell without depending on `armnet` or MiamiDX.

### `amigaos/src/armnet.device.c`

SANA-II network device for AmigaOS. It exposes the sidecARM shared-memory network transport as a normal Amiga network device that can be used by MiamiDX.

Ethernet frames are exchanged with the Linux `armnet` driver through shared RAM. The current implementation services receive traffic from the VBlank interrupt and requires no TAP device or userspace forwarding component.

### `amigaos/src/armnet_shm.h`

Defines the shared-memory ABI used by the AmigaOS side of `armnet`.

It contains the common ring-buffer layout, packet slots, constants and flags that must remain compatible with the equivalent Linux-side definitions. Any incompatible change here would break communication between `armnet.device` and the Linux driver.

---

## Linux ARM64

### `linux-arm64/armnet-kernel/armnet.c`

Linux kernel network driver for sidecARM. It creates the `arm0` Ethernet interface and exchanges raw Ethernet frames with AmigaOS through the shared-memory transport.

Together with `armnet.device`, it forms the direct Linux-to-Amiga networking path used by MiamiDX, without requiring a virtual TAP interface or an intermediate userspace relay.

### `linux-arm64/armnet-kernel/armnet_shm.h`

Defines the Linux-side shared-memory structures used by `armnet`.

It describes the transmit and receive rings, packet slots, flags and transport constants. Its layout must remain synchronized with the AmigaOS `armnet_shm.h`.

### `linux-arm64/armblk-kernel/armblk.c`

Linux block-device driver that exposes the Amiga-hosted backing image as `/dev/armblk0`.

Linux sees a normal block device, while the actual image file remains managed by AmigaOS. Requests are transferred through shared memory to the backend embedded in the AmigaOS `Linux` launcher.

This device is now the persistent Linux system disk and is used as the real root filesystem after the early initramfs stage.

### `linux-arm64/armblk-kernel/armblk_shm.h`

Defines the shared-memory protocol used by `armblk`.

It contains the request queue, operation types, status values, transfer buffers and shared state used by both the Linux kernel driver and the AmigaOS backend.

### `linux-arm64/armterm-kernel/armterm.c`

Current Linux kernel TTY driver for ArmTerm.

It exposes the shared-memory console directly as a Linux terminal device, removing the need for the old `armterm-relay` userspace process. This makes ArmTerm part of the normal Linux TTY infrastructure and significantly simplifies the boot architecture.

### `linux-arm64/armterm-kernel/armterm_shm.h`

Defines the shared-memory transport used by the ArmTerm TTY.

It describes the console buffers, control state and synchronization data shared between the Linux kernel driver and the AmigaOS ArmTerm client.

### `linux-arm64/pinux-poc7-armblk-armnet-armterm-128M.dts`

Current Device Tree source for the sidecARM Linux environment.

It describes the ARM64 Linux memory layout and the platform devices used by `armblk`, `armnet` and `armterm`. The service area is designed around the current **128 MB carveout**, while the AmigaOS launcher patches the final physical addresses dynamically before Linux starts.

---

## Minimal Initramfs

### `linux-arm64/initramfs-armblk-minimal/init`

Minimal PID 1 bootstrap used before the persistent Linux root filesystem becomes available.

It mounts the required pseudo-filesystems, waits for `/dev/armblk0`, mounts it as the new root, moves `/proc`, `/sys` and `/dev`, then performs `switch_root` into the Alpine installation stored on the block image.

Its role is intentionally small: it only bridges early kernel boot to the real persistent system disk.

### `linux-arm64/initramfs-armblk-minimal/make-initramfs.sh`

Build script for the minimal sidecARM initramfs.

It packages the early userspace environment required to reach `/dev/armblk0` and perform the root switch. Older full Alpine initramfs builders are no longer part of the current architecture.

---

## Linux Kernel Patch

### `linux-arm64/kernel-patches/irq-bcm2836-physical-core1-poc.patch`

Critical Linux kernel patch required by the unusual sidecARM CPU topology.

Linux runs as logical CPU0 while actually executing on Raspberry Pi **physical core 1**. The stock BCM2836 per-CPU interrupt code assumes logical and physical CPU numbering match, so local interrupts would otherwise be routed to the wrong core.

This patch corrects that mismatch and is required for reliable timers, sleeps, network timeouts and other per-CPU interrupt-driven kernel functionality.

---

## Emu68 Modifications

### `emu68-patch/armservice.c`

Core Emu68 implementation of the sidecARM ARM service.

It manages the reserved Linux service memory and the CPU1 control path used during startup and handoff. It is the central bridge between Emu68 running AmigaOS on CPU0 and Linux running independently on CPU1.

### `emu68-patch/start.c`

Integrates the ARM service into the Emu68 startup path and implements the CPU1 Linux handoff sequence.

It prepares the physical core for Linux, synchronizes the shared service memory and transfers execution from the Emu68 service loop to the Linux kernel while leaving AmigaOS running on CPU0.

### `emu68-patch/M68k_LINE4.c`

Adds the custom MOVEC interface used by the AmigaOS launcher to communicate with the ARM service.

The added control registers expose commands, status values, Linux entry and DTB offsets, and the physical base address of the reserved service memory. This is the low-level control channel used by `Linux.c` during startup.

### `emu68-patch/mmu.c`

Contains the MMU-side changes required by the sidecARM reserved-memory arrangement.

Its purpose is to ensure that the Linux service region and the relevant shared-memory areas are mapped consistently and remain accessible through the address layout expected by both Emu68 and the sidecARM components.

### `emu68-patch/CMakeLists.txt`

Build-system changes required to include the sidecARM Emu68 components.

It adds the ARM service source files to the Emu68 build so the CPU1 Linux support and its shared-memory infrastructure are linked into the resulting Emu68 image.
