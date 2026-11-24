section .rodata
align 16
global cornea_font_blob
global cornea_font_blob_end
cornea_font_blob:
incbin "cornea-fonts.bin"
cornea_font_blob_end:

section .note.GNU-stack noalloc noexec nowrite
