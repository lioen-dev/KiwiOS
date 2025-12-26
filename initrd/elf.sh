for file in *.c; do
    rm -f "${file%.c}"
    echo "Compiling $file..."
    x86_64-elf-gcc -std=c11 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -O2 -nostdlib -static -Wl,--gc-sections -o "${file%.c}" "$file"
done