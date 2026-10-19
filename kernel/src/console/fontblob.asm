section .rodata
align 16
global corona_font_blob
global corona_font_blob_end
corona_font_blob:
incbin "corona-fonts.bin"
corona_font_blob_end:

section .note.GNU-stack noalloc noexec nowrite
