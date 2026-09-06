## Filesystem, maintenance and system administration

* Edit the live Amiga Startup-Sequence directly from Linux with `nano`, without copying it to another machine.
* Edit User-Startup and other Amiga configuration files from Linux.
* Use Linux as an emergency or maintenance console for AmigaOS.
* Search the entire Amiga filesystem recursively with `grep`.
* Locate `.device`, `.library`, AHI-related files, large files, and other targets with `find`.
* Perform controlled bulk text changes across Amiga scripts and configuration files with `sed`, `grep`, `awk`, and related tools.
* Compare configuration files, backups, and directory trees with `diff`.
* Calculate and verify checksums of Amiga files with `sha256sum`.
* Create integrity manifests for an Amiga system and verify them later.
* Inspect files and executables with `file`, `strings`, `hexdump`, and `xxd`.
* Inspect suspicious files or malware candidates without first copying them to an external computer.
* Use Linux as an embedded recovery environment to repair a broken Startup-Sequence, move faulty drivers, or restore configuration files.
* Create automatic backups before risky configuration experiments.
* Create snapshot-style backups of `S:`, `DEVS:`, `L:`, `LIBS:`, `Prefs/` and other critical directories.

## Compression, archives, downloads and modern protocols

* Compress Amiga directories on the ARM64 CPU using `tar`, `gzip`, `xz`, `zstd`, or `bzip2`, leaving the emulated 68k CPU free.
* Extract ZIP, TAR and similar archives directly onto an Amiga volume.
* Download files with `wget` or `curl` directly into `RAM:`, `Work:`, or another Amiga volume.
* Use Linux as a modern download engine for AmigaOS.
* Let Linux terminate modern HTTPS/TLS connections and write the resulting files directly into Amiga storage.
* Use Linux as an Amiga package-installation backend: download, verify checksums, unpack archives, copy files into the right directories, and keep an installation log.
* Create a modern Aminet client that searches, downloads over HTTPS, unpacks, and places files directly in `RAM:` or `Work:Downloads`.

## Development and source-code workflows

* Use Git directly on a source tree stored on an Amiga volume for status, diff, log, and potentially checkout/pull operations.
* Cross-compile Amiga software with `m68k-amigaos-gcc` on ARM Linux while reading source from, and writing binaries directly to, the Amiga filesystem.
* Run a complete edit-build-test loop inside the PiStorm: edit on Amiga or Linux, compile on ARM Linux, and execute immediately under AmigaOS.
* Use Python scripts to read, transform, and write native Amiga files.
* Generate Amiga configuration files programmatically from Linux.
* Generate AmiDock configurations, WHDLoad launch lists, package indexes, playlists, ROM lists, emulator configurations, and development manifests.
* Format Amiga-hosted source code with `clang-format`.
* Lint and statically analyse Amiga-hosted source code with `cppcheck`, `clang`, `sparse`, or custom Python tools.
* Search large Amiga source trees quickly with `ripgrep`.
* Generate documentation such as HTML, PDF, man pages, and documentation indexes directly from Amiga source trees.
* Generate release archives from source trees stored on the Amiga, with compression performed by the ARM CPU.
* Inspect Amiga executables with generic Unix tools and potentially specialised Hunk-format analysis utilities.

## Media and document services

* Convert audio and video with `ffmpeg` and write the result directly into Amiga storage.
* Convert images with ImageMagick or similar tools, producing formats such as PNG or ILBM directly for Amiga use.
* Render PDFs on Linux and place page images directly in Amiga `RAM:` or another Amiga directory.
* Convert modern image formats such as WebP, AVIF, HEIC, SVG, and modern PNG/JPEG variants into formats understood by Amiga applications.
* Use Linux spell-checking and modern language tools on text files stored on the Amiga.

## Backup, remote access and protocol gateways

* Back up Amiga volumes incrementally with `rsync`.
* Back up Amiga data to another Linux host, a NAS, SSH storage, future USB storage, or cloud-backed directories.
* Back up to a remote SSH server without requiring an SSH implementation on AmigaOS.
* SSH into the Linux service and administer the live Amiga filesystem remotely.
* Copy files from a modern computer directly into an Amiga volume through SCP → Linux → Fitz → Amiga.
* Expose the Amiga filesystem through SFTP without running an SFTP server on AmigaOS.
* Expose Amiga storage to Windows through modern Samba/SMB running on Linux.
* Expose Amiga volumes to Unix systems through NFS.
* Expose Amiga directories through WebDAV to Windows, macOS, or mobile clients.
* Serve the Amiga filesystem over HTTP using Linux.
* Build a browser-based PiStorm/Amiga management UI exposing files, logs, startup scripts, Emu68 diagnostics, backups, downloads, screenshots, and ROM sets.

## Bidirectional filesystem integration and UX

* Mount Amiga `SYS:`, `Work:`, and `RAM:` under Linux as ordinary directories.
* Export Linux storage back to AmigaOS and mount it as a native-looking `LINUX:` volume.
* Let Workbench immediately see files generated or downloaded by Linux.
* Create a true dual-environment machine in which AmigaOS and Linux are alive simultaneously and can see each other's files.
* Use Fitz service discovery or explicit addressing to make filesystem mounting nearly configuration-free.
* Reconnect or retry mounts automatically when the Amiga-side Fitz server or Miami networking becomes available.
* Use a convenience command such as `amiga nano SYS:S/Startup-Sequence` that transparently translates Amiga paths into Linux paths.
* Translate Amiga paths such as `SYS:S/Startup-Sequence` or `RAM:test` into their `/mnt/amiga` equivalents.
* Mount `/mnt/amiga` automatically during Linux boot.
* Start the Fitz services for `SYS:`, `Work:`, and `RAM:` automatically from AmigaOS startup.
* Provide a mature user experience where the user can move between an Amiga shell, MiamiTelnet, `/mnt/amiga`, and `LINUX:` without needing to understand shared memory, FUSE, or ARM-core details.

## Indexing and filesystem-triggered services

* Maintain an index of the Amiga filesystem on Linux for near-instant filename searches.
* Provide full-text search across Amiga documents using `ripgrep`, SQLite FTS, Recoll, or similar tools.
* Trigger Linux jobs when files appear in designated Amiga directories, creating a filesystem-based RPC mechanism.
* Use `RAM:ARM/in` and `RAM:ARM/out` as generic request/result queues for ARM-side processing.
* Implement a PDF drop folder: copy a PDF into `RAM:ARM/PDF/` and receive rendered pages back in an Amiga-readable format.
* Implement an image-conversion drop folder: copy a WebP or other modern image into `RAM:ARM/Convert/` and receive PNG or ILBM output.
* Implement a URL downloader: place a URL in a request file and let Linux download the target into `RAM:Downloads/`.

## Core shared-filesystem demonstrations

* Expose Amiga `RAM:` to Linux as a safe initial live read/write filesystem.
* Create a file from Linux and read it immediately from AmigaOS.
* Create a file from AmigaOS and read it immediately from Linux.
* Edit a live Amiga file interactively from Linux with `nano`.
* Exercise directory-heavy operations including `find`, `grep`, `mkdir`, rename, and delete.
* Mount a real Amiga disk read-only for traversal, searching, checksums, and archiving before enabling writes.
* Perform controlled writes on a disposable directory of a real Amiga volume, including create, overwrite, truncate, append, rename, delete, nested directories, and large files.
