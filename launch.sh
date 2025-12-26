#!/bin/bash
set -e

WIN_IMAGE=$(wslpath -w "$(readlink -f kiwiOS.iso)")
WIN_EXT2_IMG=$(wslpath -w "$(readlink -f kiwi.img)")

cd "/mnt/c/Program Files/qemu/"

./qemu-system-x86_64.exe \
  -M q35 \
  -serial stdio \
  -device ich9-ahci,id=ahci0 \
  -drive id=disk,file="$WIN_EXT2_IMG",if=none,format=raw,media=disk \
  -device ide-hd,drive=disk,bus=ahci0.0 \
  -device ich9-intel-hda \
  -device hda-duplex \
  -cdrom "$WIN_IMAGE" \
  -boot order=d \
 