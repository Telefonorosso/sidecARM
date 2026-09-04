#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK="$HERE/root"
OUT="$HERE/initramfs-armblk.cpio.gz"
rm -rf "$WORK"
mkdir -p "$WORK/bin" "$WORK/dev" "$WORK/proc" "$WORK/sys" "$WORK/newroot"
cp "$HERE/busybox" "$WORK/bin/busybox"
cp "$HERE/init" "$WORK/init"
chmod 755 "$WORK/bin/busybox" "$WORK/init"
(
  cd "$WORK"
  find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "$OUT"
)
rm -rf "$WORK"
echo "Built: $OUT"
ls -lh "$OUT"
