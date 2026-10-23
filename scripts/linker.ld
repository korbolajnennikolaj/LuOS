/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 korbolajnennikolaj */

/* scripts/linker.ld */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
}

SECTIONS
{
    /* Move into the High Half (upper half of memory) */
    . = 0xffffffff80000000;

    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);

    .initfs : {
        __initfs_start = .;
        KEEP(*(.initfs))
        __initfs_end = .;
    } :rodata

    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
        
        /* IMPORTANT: Explicitly keep the Limine requests! */
        KEEP(*(.requests_start_marker))
        KEEP(*(.requests))
        KEEP(*(.requests_end_marker))

        . = ALIGN(64);
        __start_lkpi_percpu = .;
        KEEP(*(lkpi_percpu))
        __stop_lkpi_percpu = .;
        . = ALIGN(8);
        __start_lkpi_initcall = .;
        KEEP(*(lkpi_initcall))
        __stop_lkpi_initcall = .;
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        __kernel_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}