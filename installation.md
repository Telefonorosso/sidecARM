## PREREQUISITES

* Pistorm CLASSIC (Amiga 600 or 1200)
* Raspberry Pi3A+
* AmigaOS 3.1
* This guide is based on a standard Emu68-Imager installation with Miami
* You will need to unlock your Miami to the DELUXE version!
* https://eab.abime.net/showthread.php?t=16697&highlight=miami+keys
* (copy the 3 files in miami:)
* (without MiamiDX you will be very soon bored with sidecARM...)


## Step-by-step installation

* Download the sidecARM 1.2 release package:
https://github.com/Telefonorosso/sidecARM/releases/download/1.2/sidecARM1.2.zip

* Extract `sidecARM1.2.zip` to your desktop.

* Inside the `bundle` directory, extract `armblk0.7z`

* If needed, download 7-Zip from:
  https://www.7-zip.org/download.html

* After extraction, you should have a 256 megabytes `armblk0.img` file.

* Keep the original `armblk0.7z` archive as an easy way to restore a clean Linux disk image later.

* Shut down the Amiga completely.

* Remove the PiStorm SD card and connect it to your Windows PC.

* Copy the `bundle` directory to the SD card.

* You may rename the `bundle` directory to `ARM` if you wish.

* **Make a backup copy of the Emu68 kernel image currently used by your system!**

* Replace it with the supplied `Emu68.img.gz`, or modify `config.txt` so that Emu68 points to the supplied kernel image.

* Safely eject the SD card from Windows.

* Put the SD card back into the PiStorm and start the Amiga.

* Copy the supplied `armnet.device` to:
  `DEVS:Networks/`

* **Make a backup copy of:
  `Miami:WIFIPI.default`**

* Replace it with the sidecARM version supplied in the package.

* Open an AmigaOS Shell.

* Change directory to `SD0:ARM/`

* Start the ARM64 Linux service with:
  `Linux linux-arm64.img sidecarm.dtb initramfs.cpio.gz armblk0.img`

* Switch to the graphical console!!!
  `ArmTerm -fb`

* Wait the boot process to complete, then login with root / alpine

* From the Linux shell, try a few basic commands to verify that the system is running correctly:
  `ip addr`
  `dmesg | tail`
  `whoami`
  `ls`

* Go back to Workbench by pressing CTRL+ESC!!!

* Run the PiStorm supplied `ONLINE` helper to bring up WiFi

* Open MiamiDX.

* Go to the `Interfaces` section.

* Bring the `armnet` interface online.

* Return to the graphical console by re-launching `ArmTerm -fb`

* Verify the Amiga/Linux link with:
  `ping -c3 10.68.0.1`

* Press `CTRL+ESC` again.

* You can now connect to Linux from AmigaOS using:
  `Miami:MiamiTelnet 10.68.0.2`

* To reach Linux directly from Windows, open Command Prompt as Administrator.

* Add a route to the sidecARM Linux network:
  `route -p add 10.68.0.0 mask 255.255.255.252 <AMIGA_IP>`

* Replace `<AMIGA_IP>` with the actual address of your Amiga.

* **The `-p` option makes the Windows route persistent. Without it, the route must be added again after rebooting Windows.**

* You can now connect from Windows using PuTTY to:
  `10.68.0.2`

* Use either Telnet or SSH (root / alpine)

* To access Amiga files from Linux install Fitz on the Amiga side:
  https://aminet.net/package/comm/tcp/Fitz

* Amiga: `fitz serve SDH0:`

* Linux: `fitz-mount 10.68.0.1 /mnt/amiga/`

* `nano /mnt/amiga/s/Startup-Sequence`

* CTRL+X

* When you want to go back to your normal life:

* `umount /mnt/amiga`

* Stop fitz by pressing CTRL+C.

* Cleanly shut off Linux:
  `endcli`
  (I know, it's confusing)

* Go back to your normal life.
