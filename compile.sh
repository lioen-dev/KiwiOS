#!/bin/bash
rm -rf bin iso_root obj kiwi.img kiwiOS.iso
bash rmindentifier.sh

# Compile filesystem initrd
cd initrd
bash elf.sh
cd ..

bash ext2.sh initrd kiwi.img

make
# Download the latest Limine binary release for the 10.x branch, only if it doesn't exist.

if [ ! -d "limine" ]; then
  echo "[*] Cloning Limine bootloader..."
  git clone https://codeberg.org/Limine/Limine.git limine --branch=v10.x-binary --depth=1
fi

# Build "limine" utility.
make -C limine

# Create a directory which will be our ISO root.
mkdir -p iso_root

# Copy the relevant files over.
mkdir -p iso_root/boot
cp -v bin/kiwiOS iso_root/boot/
mkdir -p iso_root/boot/limine
cp -v limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
      limine/limine-uefi-cd.bin iso_root/boot/limine/

# Create the EFI boot tree and copy Limine's EFI executables over.
mkdir -p iso_root/EFI/BOOT
cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/

# Create the bootable ISO.
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso_root -o kiwiOS.iso

# Install Limine stage 1 and 2 for legacy BIOS boot.
./limine/limine bios-install kiwiOS.iso

bash launch.sh