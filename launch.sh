#!/bin/bash
set -e

WIN_IMAGE=$(wslpath -w "$(readlink -f kiwiOS.iso)")

cd "/mnt/c/Program Files/qemu/"

./qemu-system-x86_64.exe \
  -M q35 \
  -serial stdio \
  -device ich9-ahci,id=ahci0 \
  -cdrom "$WIN_IMAGE" \
  -boot order=d \
 