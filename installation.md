## Step-by-step installation

* Download the sidecARM 1.2 release package:
  `https://github.com/Telefonorosso/sidecARM/releases/download/1.0/sidecARM1.2.zip`

* Extract `sidecARM1.2.zip`.

* Inside the extracted directory, extract `armblk0.7z`.

* If needed, download 7-Zip from:
  `https://www.7-zip.org/download.html`

* After extraction, you should have an `armblk0.img` file of approximately 262,144 KB.

* Keep the original `armblk0.7z` archive as a backup if you want an easy way to restore a clean Linux disk image later.

* Shut down the Amiga completely.

* Remove the PiStorm SD card and connect it to your Windows PC.

* Copy the `bundle` directory from the sidecARM package to the SD card.

* You may rename the `bundle` directory if you prefer; for example, `ARM`.

* Make a backup copy of the Emu68 kernel image currently used by your system.

* Replace it with the supplied `Emu68.img.gz`, or modify `config.txt` so that Emu68 points to the supplied kernel image.

* Safely eject the SD card from Windows.

* Put the SD card back into the PiStorm and start the Amiga.

* Copy `armnet.device` to:
  `DEVS:Networks/`

* Make a backup copy of:
  `Miami:WIFIPI.default`

* Replace it with the sidecARM version supplied in the package.

* Open an AmigaOS Shell.

* Change directory to the directory you copied to the SD card. For example:
  `SD0:ARM`

* Start the ARM64 Linux service with:
  `Linux linux-arm64.img sidecarm.dtb initramfs.cpio.gz armblk0.img`

* Switch to the graphical console!!!
  `ArmTerm -fb`

* From the Linux shell, try a few basic commands to verify that the system is running correctly:
  `ip addr`
  `dmesg | tail`
  `whoami`
  `ls`

* Go back to Workbench by pressing CTRL+ESC!!!

* Run the PiStorm supplied `ONLINE` helper to bring up WiFi

* Open MiamiDX.

* Go to the **Interfaces** section.

* Bring the `armnet` interface online.

* Return to the graphical console by pressing F2

* Verify the Amiga/Linux link with:
  `ping 10.68.0.1`

* Press `Ctrl+Esc` again

* You can now connect to Linux from AmigaOS using:
  `Miami:MiamiTelnet 10.68.0.2`

* To reach Linux directly from Windows, open **Command Prompt as Administrator**.

* Add a route to the sidecARM Linux network:
  `route -p add 10.68.0.0 mask 255.255.255.252 <AMIGA_LAN_IP>`

* Replace `<AMIGA_LAN_IP>` with the normal LAN address of your Amiga.

* The `-p` option makes the Windows route persistent. Without it, the route must be added again after rebooting Windows.

* You can now connect from Windows using PuTTY to:
  `10.68.0.2`

* Use either **Telnet** or **SSH**.

* For SSH, log in as:
  `root`

* Default password:
  `alpine`

* To access Amiga files from Linux install Fitz on the Amiga side:
  `https://aminet.net/package/comm/tcp/Fitz`

* Amiga: `fitz serve SDH0:`

* Linuz `fitz-mount 10.68.0.1 /mnt/amiga/`

* `nano /mnt/amiga/s/Startup-Sequence`

* When you want to go back to your normal life:

* `umount /mnt/amiga`

* Stop fitz by pressing CTRL+C

* Cleanly turn off Linux:
  `endcli` (I know, it's confusing)

  
