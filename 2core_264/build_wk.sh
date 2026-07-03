#!/bin/bash
set -e

echo "Compiling edge264 using 2Cores ..."

# Gemeinsame Flags
CXXFLAGS="-c -m32 -ffreestanding -fno-builtin -nostdlib -msse4.2 -msse4.1 -mssse3 -mpopcnt  -mfpmath=sse -O3 -ffast-math -ftree-vectorize -flax-vector-conversions -DDOS2CORE -I../include -I."
CFLAGS="-c -m32 -ffreestanding -fno-builtin -nostdlib -msse -msse2 -msse3 -mfpmath=sse -fPIC -fmerge-all-constants -O3 -g -ffast-math -ftree-vectorize -flax-vector-conversions -DDOS2CORE -I../include -I."
LIBC_CFLAGS="-c -m32 -ffreestanding -fno-builtin -nostdlib -fPIC -O3 -I../include -I."

i586-pc-msdosdjgpp-g++ $CXXFLAGS edge_cpu2.cc -o edge_cpu2.o
i586-pc-msdosdjgpp-g++ $CXXFLAGS libc_stubs.cc -o libc_stubs.o
i586-pc-msdosdjgpp-gcc $CFLAGS ../../edge264-dos/src/edge264.c -o edge_core.o

# ============================================================================
# Linker-Script inline.
#  * . = 0x0           -> Image auf Link-Base 0 (Segment-Modell addiert workerPhys)
#  * .text.entry first -> cpu2_decoder_loop liegt garantiert auf Offset 0
#  * .rodata.*         -> WICHTIG: faengt .rodata.str1.*, .rodata.cst16 (SIMD-Masken!)
#  * .bss + COMMON in .data ziehen -> wird als PROGBITS NULL-gefuellt mit ins
#    Binary geschrieben (kein crt0 nullt bss!). So ist bss geladen UND genullt.
# ============================================================================
cat > linkFile.ld << 'LDEOF'
SECTIONS {
  . = 0x0;
  .text : {
    KEEP(*(.text.entry))
    *(.text)
    *(.text.*)
  }
  .rodata : {
    *(.rodata)
    *(.rodata.*)
  }
  .data : {
    *(.data)
    *(.data.*)
    /* bss + COMMON in die geladene, nullgefuellte Region (es gibt kein crt0) */
    __bss_start = .;
    *(.bss)
    *(.bss.*)
    *(COMMON)
    __bss_end = .;
  }
}
LDEOF


echo "Linking objects..."
i586-pc-msdosdjgpp-ld -T linkFile.ld --entry _cpu2_decoder_loop edge_cpu2.o libc_stubs.o edge_core.o -o edge_cpu2.coff

echo "Creating raw binary payload..."
i586-pc-msdosdjgpp-objcopy -O binary edge_cpu2.coff edge_wk.bin

