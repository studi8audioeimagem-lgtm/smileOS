#ifndef FONT_ATLAS_H
#define FONT_ATLAS_H

#include "types.h"

typedef struct {
    u16 x;
    u16 y;
    u16 w;
    u16 h;
    i16 xoff;
    i16 yoff;      /* from top of line (positive down) */
    u16 xadvance;  /* pixels */
} font_glyph_t;

typedef struct {
    u32 atlas_w;
    u32 atlas_h;
    const u8 *atlas_alpha;        /* atlas_w * atlas_h */
    const font_glyph_t *glyphs;   /* 256 */
    u32 line_height;
    u32 ascent;
    u32 cell_advance;             /* monospace cell width in px */
} font_face_t;

/* Generated faces (baked at build time). */
extern const font_face_t g_font_face_ui;
extern const font_face_t g_font_face_boot;

#endif

