/* web/faces.html linked in like ESP-IDF's EMBED_FILES */
    .section .rodata.faces_html, "a"
    .global _binary_faces_html_start
    .global _binary_faces_html_end
_binary_faces_html_start:
    .incbin FACES_HTML
_binary_faces_html_end:
    .byte 0
