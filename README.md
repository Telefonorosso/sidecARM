# sidecARM

ARM64 Linux running alongside AmigaOS on Emu68/PiStorm.

sidecARM is an experimental ARM64 Linux service environment running concurrently with AmigaOS.

AmigaOS remains the primary operating system under Emu68, while Linux runs on another ARM core and provides a modern Linux environment, networking, storage and services.

Current tested Emu68 baseline:

```text
9b4379a
```

## Current proof of concept

The current sidecARM POC provides:

- ARM64 Alpine Linux running alongside AmigaOS
- Linux running on physical Cortex-A53 CPU1
- a real Linux root filesystem on `/dev/armblk0`
- shared-memory networking through ARMNET
- MiamiDX integration
- ArmTerm local console
- normal Alpine package management
- Telnet access over ARMNET
- SSH access over ARMNET
- read/write access to AmigaOS files through Fitz

The current target platform is:

- PiStorm Classic
- Raspberry Pi 3A+
- AmigaOS 3.x
- Emu68
- MiamiDX

---

# Architecture

AmigaOS continues running normally under Emu68.

Linux runs independently on ARM CPU1 using reserved RAM.

The two environments communicate through dedicated sidecARM interfaces.

For networking:

```text
AmigaOS                  Linux

armnet.device  <------>  arm0
10.68.0.1                10.68.0.2
```

Netmask:

```text
255.255.255.252
```

Linux uses `/dev/armblk0` as its system disk.

---

# Linux system disk: /dev/armblk0

`/dev/armblk0` is now the main Linux system disk.

It contains the Alpine Linux root filesystem, including normal persistent Linux directories such as:

```text
/etc
/root
/usr
/var
```

This means that installed packages, configuration files and normal filesystem changes survive a reboot.

A prepared `armblk0.img` is supplied with sidecARM in ZIP format.

There is therefore **no need to create or format the Linux system disk manually**.

Extract the supplied archive and copy `armblk0.img` to the `EMU68BOOT` partition.

`EMU68BOOT` is used because it is directly accessible from Windows and is the natural location for the sidecARM boot files.

---

# First test: ArmTerm

**ArmTerm should be the first test performed after starting Linux.**

Start Linux from AmigaOS, then wait approximately:

```text
15-20 seconds
```

before launching:

```text
ArmTerm
```

ArmTerm provides a direct interactive Linux console through shared memory and does **not** require ARMNET or MiamiDX networking.

If the Linux shell appears, the essential sidecARM boot path is working:

```text
Emu68
  -> CPU1
  -> ARM64 Linux
  -> /dev/armblk0 root filesystem
  -> Linux userspace
  -> ArmTerm
```

Useful first commands are:

```sh
uname -a
df -h
mount
ip addr
dmesg | tail
```

Only after ArmTerm has been validated should ARMNET and MiamiDX networking be tested.

---

# AmigaOS networking

sidecARM requires **MiamiDX**.

The standard Miami version is not sufficient for the intended configuration because sidecARM relies on MiamiDX support for:

- multiple simultaneous network interfaces
- IP routing
- IP-NAT

The current development system uses a 3Com EtherLink III PCMCIA adapter for the normal AmigaOS LAN connection.

ARMNET is an additional private interface dedicated to communication between AmigaOS and sidecARM Linux.

```text
3Com EtherLink III
    AmigaOS LAN / Internet

armnet.device
    AmigaOS <-> Linux
```

The ARMNET addresses are:

```text
AmigaOS: 10.68.0.1
Linux:   10.68.0.2
Netmask: 255.255.255.252
```

---

# Ready-to-use MiamiDX configuration

The repository includes a modified:

```text
WIFIPI.default.miami
```

configuration compatible with sidecARM.

It is based on the normal PiStorm MiamiDX configuration and already contains the changes required for ARMNET.

This is the recommended starting point instead of manually recreating the complete MiamiDX configuration.

The normal PiStorm/Amiga network interface remains active while `armnet.device` provides the private sidecARM link.

After Linux has booted and ArmTerm has been successfully tested, ARMNET can be checked from AmigaOS with:

```text
ping 10.68.0.2
```

From Linux:

```sh
ping 10.68.0.1
```

---

# Remote Linux access

Once ARMNET is working, Linux can be accessed remotely.

## SSH

SSH is available on the standard port 22, root login is enabled.

Default credentials:

```text
user: root
password: alpine
```

## Telnet

Telnet access is also available for testing and compatibility with AmigaOS terminal software.

---

# Accessing Linux from another computer

The Amiga can act as the route toward the private sidecARM subnet.

For example, on Windows a persistent route can be added with:

```cmd
route -p add 10.68.0.0 mask 255.255.255.252 <AMIGA_LAN_IP>
```

Replace:

```text
<AMIGA_LAN_IP>
```

with the normal LAN address of the Amiga.

Linux can then be reached directly at 10.68.0.2 with Putty (ssh or telnet).

---

# Accessing AmigaOS files from Linux

sidecARM uses Fitz when Linux needs access to real AmigaOS files and directories.

For example, start a Fitz server on AmigaOS:

```text
fitz serve Work:
```

Then from Linux:

```sh
fitz-mount 10.68.0.1 /mnt/amiga
```

The AmigaOS filesystem becomes available under:

```text
/mnt/amiga
```

Normal Linux commands can then operate on AmigaOS files:

```sh
ls /mnt/amiga
nano /mnt/amiga/test.txt
cp file /mnt/amiga/
mkdir /mnt/amiga/testdir
```

Fitz and `/dev/armblk0` have different purposes:

```text
/dev/armblk0
    Linux system disk

Fitz
    access to real AmigaOS files
```

---

# Supplied files

The repository contains the files required for the tested proof of concept, including:

- sidecARM-enabled `Emu68.img`
- AmigaOS `Linux` launcher
- `ArmTerm`
- `armnet.device`
- ARM64 Linux kernel
- device tree
- Alpine boot environment
- prepared `/dev/armblk0` Linux system disk in ZIP format
- ARMNET support
- modified `WIFIPI.default.miami` configuration

The supplied Emu68 build is based on:

```text
9b4379a
```

Do not assume that the current sidecARM modifications can be applied unchanged to arbitrary Emu68 versions.

---

# Step by step installation

**TODO — STEP BY STEP INSTALLATION**

This section will contain the complete installation procedure from a standard PiStorm/Emu68 setup to the first successful sidecARM boot.

---

# Recommended validation order

For the current POC, test components in this order:

```text
1. Boot AmigaOS with the supplied Emu68
2. Start Linux
3. Wait 15-20 seconds
4. Run ArmTerm
5. Verify the Linux shell
6. Verify /dev/armblk0
7. Load/use the supplied WIFIPI.default.miami configuration
8. Test ARMNET
9. Test SSH
10. Test Fitz
```

ArmTerm deliberately comes before networking: it provides the simplest possible proof that Linux itself has booted correctly.

---

# Experimental status

sidecARM is experimental software.

It modifies low-level Emu68 behaviour, ARM CPU startup, reserved memory and communication between AmigaOS and ARM64 Linux.

The current release specifically targets the tested PiStorm Classic + Raspberry Pi 3A+ configuration.

Keep backups of important AmigaOS data.

Current Emu68 reference baseline:

```text
9b4379a
```
