#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <INITRD_DIR> [OUT_IMG]"; exit 1
fi

DIR="$(readlink -f "$1")"
IMG="${2:-kiwi-ext2.img}"

if ! command -v genext2fs >/dev/null; then
  echo "Installing genext2fs (requires sudo)..."
  sudo apt-get update && sudo apt-get install -y genext2fs e2fsprogs
fi

[[ -d "$DIR" ]] || { echo "Directory not found: $DIR"; exit 1; }

# pick an ext2 block size; 1024 is widely compatible
BLKSZ=1024

# measure size (KiB) and add ~20% headroom + 20 MiB extra
SIZE_K=$(du -sk --apparent-size "$DIR" | cut -f1)
HEADROOM_K=$(( (SIZE_K*12)/10 + 20*1024 ))

# blocks = ceil((HEADROOM_K * 1024) / BLKSZ)
BLOCKS=$(( (HEADROOM_K*1024 + BLKSZ - 1) / BLKSZ ))

echo "[*] Building ext2 image from $DIR"
echo "    Block size: $BLKSZ, Blocks: $BLOCKS (~$((BLOCKS*BLKSZ/1024)) KiB)"

# build the filesystem image straight from the tree
genext2fs -B "$BLKSZ" -b "$BLOCKS" -d "$DIR" "$IMG"

# quick sanity (won't mount, just checks)
e2fsck -fy "$IMG" || true

echo "[*] Done. Created $IMG"
echo
echo "Run QEMU (boot kernel via your ISO, attach this as an IDE data disk):"
echo "qemu-system-x86_64 -m 1024 -serial stdio \\"
echo "  -drive file=$IMG,if=ide,format=raw,media=disk \\"
echo "  -cdrom kiwi.iso -boot d"
