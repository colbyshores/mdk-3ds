@ MDK's own shader binary, byte for byte (see main.c).
    .section .rodata
    .align 4
    .global mdk_shbin
mdk_shbin:
    .incbin "test.shbin"
mdk_shbin_end:
    .align 4
    .global mdk_shbin_size
mdk_shbin_size:
    .word mdk_shbin_end - mdk_shbin
