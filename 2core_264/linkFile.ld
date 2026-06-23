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
