#!/bin/sh
# Builds fx3prog (the FX3 RAM program) and fx3flash with it embedded.
#   sh fx3prog/build.sh OUTDIR        (run from re/fx3/scripts)
set -e
out=${1:?output directory}
here=$(dirname "$0")
mkdir -p "$out"
arm-none-eabi-gcc -mcpu=arm926ej-s -marm -O2 -Wall -Wextra \
    -ffreestanding -fno-builtin -nostdlib -fno-pic \
    -T "$here/link.ld" -o "$out/fx3prog.elf" "$here/entry.S" "$here/fx3prog.c"
arm-none-eabi-objcopy -O binary "$out/fx3prog.elf" "$out/fx3prog.bin"
(cd "$out" && xxd -i fx3prog.bin > fx3prog_bin.h)
gcc -O2 -Wall -Wextra -DFX3PROG -I"$here" -I"$out" -o "$out/fx3flash" "$here/../fx3flash.c" \
    $(pkg-config --cflags --libs libusb-1.0)
echo "fx3prog: $(stat -c %s "$out/fx3prog.bin") bytes"
