ENTRY(_start)
SECTIONS
{
    . = 0x1000;
    
    .text : ALIGN(4) { 
        *(.text)
        *(.rodata*)
    }
    
    .data : ALIGN(4) { 
        *(.data)
    }
    
    .bss : ALIGN(4) { 
        __bss_start = .;
        *(.bss)
        *(COMMON)
        __bss_end = .;
    }
}
