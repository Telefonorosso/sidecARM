# sidecARM
ARM64 Linux running alongside AmigaOS on Emu68/PiStorm

sidecARM is an experimental ARM64 Linux service environment that runs alongside AmigaOS on Emu68/PiStorm.

AmigaOS remains the primary operating system. Linux runs on an otherwise unused ARM core and acts as a modern service processor.

The current proof of concept provides:

* ARM64 Alpine Linux running concurrently with AmigaOS
* Linux on physical Cortex-A53 CPU1
* shared-memory networking through ARMNET
* Miami/MiamiDX connectivity
* Linux package management and normal Alpine tools
* read/write access to real AmigaOS files through Fitz
* persistent Linux storage through `/dev/armblk0`

The current tested Emu68 baseline is:

```text
9b4379a
```

---

# How it works

sidecARM keeps AmigaOS and Linux deliberately separate.

AmigaOS continues running normally under Emu68.

Linux runs on CPU1 with its own reserved RAM.

Communication between them uses ARMNET, a shared-memory network interface.

The default private network is:

```text
AmigaOS: 10.68.0.1
Linux:   10.68.0.2
Netmask: 255.255.255.252
```

On AmigaOS, ARMNET appears as `armnet.device`.

On Linux, it appears as `arm0`.

To make Windows reach sidecARM Linux through the Amiga, add a persistent route for the ARMNET subnet:

```powershell
route -p add 10.68.0.0 mask 255.255.255.252 <AMIGA_LAN_IP>
```

Replace `<AMIGA_LAN_IP>` with the Amiga's 3Com/Miami address on your normal LAN.

Then use Putty (telnet) to connect to the SidecARM!

Before `armnet` is configured, AmigaOS utility **ArmTerm** provides a simple local test console between AmigaOS and the ARM64 Linux service through shared memory.
Run `ArmTerm` from AmigaOS after starting Linux to obtain an interactive Linux shell without requiring any network setup.
It is useful for initial bring-up, checking `dmesg`, testing the filesystem, and diagnosing `armnet` before TCP/IP is available.
Once `armnet` is working, normal network shells such as Telnet can be used instead.


---

# Current Amiga networking

The current test system uses **Miami/MiamiDX with a 3Com EtherLink III PCMCIA card** for normal AmigaOS LAN and Internet connectivity.

ARMNET is a second, private interface used only for communication with sidecARM Linux.

So today:

```text
3Com EtherLink III
    AmigaOS LAN / Internet

armnet.device
    AmigaOS <-> Linux
```

A future experiment may move the Raspberry Pi Wi-Fi interface to Linux and use Linux as the router/NAT gateway for AmigaOS.

That is not part of the current release.

---

# Accessing AmigaOS files

sidecARM uses **Fitz** for access to real AmigaOS files and directories.

Start a Fitz server on AmigaOS, for example:

```text
fitz serve Work:
```

Then mount it from Linux:

```bash
fitz-mount 10.68.0.1 /amiga
```

The AmigaOS files are now visible under:

```text
/amiga
```

Normal Linux tools can operate directly on them:

```bash
ls /amiga
nano /amiga/test.txt
cp file /amiga/
mkdir /amiga/testdir
```

Read/write, rename, directory creation, deletion and SHA256 verification have all been tested.

Fitz remains the intended solution for real AmigaOS filesystem access.

---

# Persistent Linux storage

Private Linux state uses a separate mechanism.

The AmigaOS-hosted file:

```text
armblk0.img
```

is exposed directly to Linux as:

```text
/dev/armblk0
```

Linux can place an ext2 filesystem on `/dev/armblk0` and use it for persistent packages, configuration and application data.

This is deliberately separate from Fitz:

```text
Fitz
    real AmigaOS files

/dev/armblk0
    private Linux persistent storage
```

This avoids using FUSE, TCP or a loop device for Linux persistence.

The underlying image remains an ordinary file stored on the Amiga-accessible boot partition.

---

# Creating armblk0.img

The easiest way is from Windows PowerShell.

Open PowerShell in any convenient directory.

For a 512 MB image:

```powershell
$size = 512MB
$fs = [System.IO.File]::Create("armblk0.img")
$fs.SetLength($size)
$fs.Close()
```

Verify it:

```powershell
Get-Item .\armblk0.img | Select-Object Name,Length
```

The expected size is:

```text
536870912 bytes
```

Copy `armblk0.img` to:

```text
EMU68BOOT
```

`EMU68BOOT` is the recommended location because it is the Emu68 boot partition and is easily accessible from Windows.

Linux will see the image through the sidecARM block transport as:

```text
/dev/armblk0
```

---

# Quick install

Everything required for the tested proof of concept is supplied with the release.

There is no need to compile Linux or rebuild the environment manually.

Current target:

* PiStorm Classic
* Raspberry Pi 3A+
* AmigaOS 3.x
* Emu68
* Miami or MiamiDX

## 1. Copy the supplied files

Copy the supplied sidecARM files to their documented locations.

The release contains the tested components, including:

* sidecARM-enabled Emu68
* AmigaOS `Linux` launcher
* `armnet.device`
* ARM64 Linux kernel
* Alpine initramfs
* device tree
* Miami Deluxe configuration file

The supplied build is based on Emu68 commit 9b4379a

## 2. Create armblk0.img

From Windows PowerShell:

```powershell
$size = 256MB
$fs = [System.IO.File]::Create("armblk0.img")
$fs.SetLength($size)
$fs.Close()
```

Copy:

```text
armblk0.img
```

to:

```text
EMU68BOOT
```

## 3. Put the Linux payload on EMU68BOOT

Place the supplied Linux payload files on `EMU68BOOT` as instructed by the release.

Using the boot partition is recommended because it is easy to update directly from Windows.

Previous testing also showed substantially better Linux-file loading performance from the SD boot area than from `SDH0:`.

## 4. Boot AmigaOS

Boot normally using the supplied sidecARM-enabled Emu68.

AmigaOS should start normally.

## 5. Start Linux

Open an AmigaShell and run:

```text
Linux linux-arm64.img sidecarm.dtb alpine.cpio.gz armblk0.img
```

The launcher loads the ARM64 kernel, initramfs and device tree and starts Linux on physical ARM CPU1.

AmigaOS remains active.

## 6. Configure ARMNET

In Miami/MiamiDX, add `armnet.device` with:

```text
IP address: 10.68.0.1
Netmask:    255.255.255.252
```

Linux uses:

```text
10.68.0.2/30
```

Test from AmigaOS:

```text
ping 10.68.0.2
```

From Linux:

```bash
ping 10.68.0.1
```

(armnet.miami is included)

The 3Com EtherLink III remains the normal AmigaOS LAN/Internet interface.

---

# Using /dev/armblk0

Once sidecARM is running, Linux should expose:

```text
/dev/armblk0
```

For a brand-new image, format it once:

```bash
mkfs.ext2 /dev/armblk0
```

Create the mount point:

```bash
mkdir -p /persist
```

Mount it:

```bash
mount /dev/armblk0 /persist
```

Test persistence:

```bash
echo "sidecARM persistent storage" > /persist/test.txt
sync
cat /persist/test.txt
```

The goal is for `/persist` to hold Linux packages, configuration and service data independently of the immutable Alpine initramfs.

---

# Performance

Current Fitz measurements are approximately:

```text
Direct Fitz I/O:      600–680 KB/s
ext2 cold reads:      ~500 KB/s
```

Small-file testing has also been successful, including hundreds of file creations and renames.

Storage location matters significantly.

In previous tests, Linux payload loading from the SD boot partition was approximately twice as fast as loading the same files from `SDH0:`.

This is one reason `EMU68BOOT` is the preferred location for sidecARM payloads and `armblk0.img`.

---

# Roadmap

Current areas of development include:

* `/dev/armblk0` optimization
* transparent persistent Alpine state
* persistent root overlay
* clean filesystem flush and shutdown
* automatic alongside boot
* moving Raspberry Pi Wi-Fi to Linux
* Linux routing/NAT for AmigaOS
* reduced startup latency
* modern Linux services callable from Amiga applications

---

# Experimental status

sidecARM is experimental software.

It modifies low-level Emu68 behavior, ARM CPU startup, memory allocation and communication between AmigaOS and Linux.

Use backups.

The supplied build currently targets:

```text
Emu68 commit 9b4379a
```

Do not assume compatibility with arbitrary Emu68 versions.

---

