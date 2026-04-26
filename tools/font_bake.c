#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal FreeType API declarations (we link against libfreetype.so.6).
   This avoids needing freetype2 development headers in the build environment. */

typedef int FT_Int;
typedef unsigned int FT_UInt;
typedef int FT_Error;
typedef int FT_Bool;
typedef long FT_F26Dot6;
typedef long FT_Fixed; /* 16.16 (FreeType uses 'long' for ABI) */
typedef long FT_Long;
typedef unsigned long FT_ULong;
typedef short FT_Short;
typedef unsigned short FT_UShort;
typedef char FT_String;

typedef struct FT_LibraryRec_ *FT_Library;
typedef struct FT_FaceRec_ *FT_Face;
typedef struct FT_GlyphRec_ *FT_Glyph;
typedef struct FT_GlyphSlotRec_ *FT_GlyphSlot;
typedef struct FT_SizeRec_ *FT_Size;
typedef struct FT_CharMapRec_ *FT_CharMap;

typedef struct {
    long x;
    long y;
} FT_Vector;

typedef struct {
    void *data;
    void (*finalizer)(void *);
} FT_Generic;

typedef struct {
    long xMin, yMin, xMax, yMax;
} FT_BBox;

typedef struct {
    uint32_t rows;
    uint32_t width;
    int32_t pitch;
    uint8_t *buffer;
    uint16_t num_grays;
    uint8_t pixel_mode;
    uint8_t palette_mode;
    void *palette;
} FT_Bitmap;

typedef struct FT_GlyphRec_ {
    void *library;
    const void *clazz;
    FT_ULong format;
    FT_Vector advance; /* 16.16 */
} FT_GlyphRec;

typedef struct {
    FT_GlyphRec root;
    FT_Int left;
    FT_Int top;
    FT_Bitmap bitmap;
} FT_BitmapGlyphRec, *FT_BitmapGlyph;

/* Public portion of FT_FaceRec (stable API). We only need up to 'glyph'. */
typedef struct FT_FaceRec_ {
    FT_Long num_faces;
    FT_Long face_index;
    FT_Long face_flags;
    FT_Long style_flags;
    FT_Long num_glyphs;
    FT_String *family_name;
    FT_String *style_name;
    FT_Int num_fixed_sizes;
    void *available_sizes;
    FT_Int num_charmaps;
    FT_CharMap *charmaps;
    FT_Generic generic;
    FT_BBox bbox;
    FT_UShort units_per_EM;
    FT_Short ascender;
    FT_Short descender;
    FT_Short height;
    FT_Short max_advance_width;
    FT_Short max_advance_height;
    FT_Short underline_position;
    FT_Short underline_thickness;
    FT_GlyphSlot glyph;
    FT_Size size;
    FT_CharMap charmap;
} FT_FaceRec;

enum {
    FT_LOAD_DEFAULT = 0x0,
};

typedef enum {
    FT_RENDER_MODE_NORMAL = 0,
} FT_Render_Mode;

extern FT_Error FT_Init_FreeType(FT_Library *alibrary);
extern FT_Error FT_Done_FreeType(FT_Library library);
extern FT_Error FT_New_Face(FT_Library library, const char *filepathname, FT_Long face_index, FT_Face *aface);
extern FT_Error FT_Done_Face(FT_Face face);
extern FT_Error FT_Set_Pixel_Sizes(FT_Face face, FT_UInt pixel_width, FT_UInt pixel_height);
extern FT_Error FT_Load_Char(FT_Face face, FT_ULong char_code, FT_Int load_flags);
extern FT_Error FT_Get_Glyph(void *slot /* FT_GlyphSlot */, FT_Glyph *aglyph);
extern FT_Error FT_Glyph_To_Bitmap(FT_Glyph *the_glyph, FT_Render_Mode render_mode, const FT_Vector *origin, FT_Bool destroy);
extern void FT_Done_Glyph(FT_Glyph glyph);

typedef struct {
    uint16_t x, y, w, h;
    int16_t xoff, yoff;
    uint16_t xadvance;
} glyph_out_t;

static void die(const char *msg) {
    fprintf(stderr, "font_bake: %s\n", msg);
    exit(1);
}

static void print_ident(FILE *f, const char *s) {
    if (!s || !*s) return;
    for (const char *p = s; *p; p++) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            fputc(c, f);
        } else {
            fputc('_', f);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <font.ttf> <px> <out.c> <name>\n", argv[0]);
        return 2;
    }

    const char *ttf_path = argv[1];
    const int px = atoi(argv[2]);
    const char *out_path = argv[3];
    const char *name = argv[4];
    if (px < 8 || px > 64) die("px out of range (8..64)");

    FT_Library lib = NULL;
    FT_Face face = NULL;
    if (FT_Init_FreeType(&lib) != 0) die("FT_Init_FreeType failed");
    if (FT_New_Face(lib, ttf_path, 0, &face) != 0) die("FT_New_Face failed");
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0) die("FT_Set_Pixel_Sizes failed");

    /* Render glyphs 0x20..0xFF into individual bitmaps, compute metrics and pack. */
    struct glyph_tmp {
        uint32_t code;
        int w, h;
        int left, top;
        int adv;
        uint8_t *bmp; /* w*h */
    } glyphs[256];
    memset(glyphs, 0, sizeof(glyphs));

    int ascent = 0;
    int descent = 0;
    int max_adv = 0;

    for (uint32_t c = 0x20; c <= 0xFF; c++) {
        if (FT_Load_Char(face, c, FT_LOAD_DEFAULT) != 0) {
            continue;
        }

        /* Access the glyph slot via the public FT_FaceRec layout. */
        FT_Glyph g = NULL;
        FT_GlyphSlot slot = ((FT_FaceRec *)face)->glyph;
        if (!slot) {
            continue;
        }
        if (FT_Get_Glyph((void *)slot, &g) != 0) {
            continue;
        }

        if (FT_Glyph_To_Bitmap(&g, FT_RENDER_MODE_NORMAL, NULL, 1) != 0) {
            if (g) FT_Done_Glyph(g);
            continue;
        }

        FT_BitmapGlyph bg = (FT_BitmapGlyph)g;
        const int w = (int)bg->bitmap.width;
        const int h = (int)bg->bitmap.rows;
        const int pitch = (int)bg->bitmap.pitch;
        const int left = (int)bg->left;
        const int top = (int)bg->top;

        /* Advance is 16.16 in FT_GlyphRec. */
        const int adv = (int)((bg->root.advance.x + 0x8000) >> 16);

        if (top > ascent) ascent = top;
        const int desc = h - top;
        if (desc > descent) descent = desc;
        if (adv > max_adv) max_adv = adv;

        uint8_t *bmp = NULL;
        if (w > 0 && h > 0 && bg->bitmap.buffer) {
            bmp = (uint8_t *)malloc((size_t)w * (size_t)h);
            if (!bmp) die("oom");
            for (int yy = 0; yy < h; yy++) {
                const uint8_t *src = bg->bitmap.buffer + (size_t)yy * (size_t)pitch;
                memcpy(bmp + (size_t)yy * (size_t)w, src, (size_t)w);
            }
        }

        glyphs[c].code = c;
        glyphs[c].w = w;
        glyphs[c].h = h;
        glyphs[c].left = left;
        glyphs[c].top = top;
        glyphs[c].adv = adv;
        glyphs[c].bmp = bmp;

        /* g is owned/destroyed by FT_Glyph_To_Bitmap with destroy=1, but the converted glyph remains valid.
           FreeType expects us to call FT_Done_Glyph on it. */
        FT_Done_Glyph((FT_Glyph)bg);
    }

    if (ascent <= 0 || max_adv <= 0) {
        die("failed to bake any glyphs (FreeType face layout mismatch in this environment)");
    }

    const int line_h = ascent + descent + 4;

    /* Pack into an atlas with a simple shelf packer. */
    const int pad = 1;
    int atlas_w = 1024;
    if (atlas_w < max_adv * 16) atlas_w = max_adv * 16;
    if (atlas_w > 2048) atlas_w = 2048;

    int x = pad, y = pad, row_h = 0;
    glyph_out_t out[256];
    memset(out, 0, sizeof(out));

    for (uint32_t c = 0x20; c <= 0xFF; c++) {
        const int gw = glyphs[c].w;
        const int gh = glyphs[c].h;
        if (gw <= 0 || gh <= 0 || !glyphs[c].bmp) {
            out[c].xadvance = (uint16_t)((c == ' ') ? (max_adv ? max_adv : px / 2) : (glyphs[c].adv > 0 ? glyphs[c].adv : max_adv));
            continue;
        }
        if (x + gw + pad >= atlas_w) {
            x = pad;
            y += row_h + pad;
            row_h = 0;
        }
        out[c].x = (uint16_t)x;
        out[c].y = (uint16_t)y;
        out[c].w = (uint16_t)gw;
        out[c].h = (uint16_t)gh;
        out[c].xoff = (int16_t)glyphs[c].left;
        out[c].yoff = (int16_t)(ascent - glyphs[c].top);
        out[c].xadvance = (uint16_t)((glyphs[c].adv > 0) ? glyphs[c].adv : max_adv);

        x += gw + pad;
        if (gh > row_h) row_h = gh;
    }

    int atlas_h = y + row_h + pad;
    if (atlas_h < 64) atlas_h = 64;

    uint8_t *atlas = (uint8_t *)calloc((size_t)atlas_w * (size_t)atlas_h, 1);
    if (!atlas) die("oom atlas");

    for (uint32_t c = 0x20; c <= 0xFF; c++) {
        const int gw = glyphs[c].w;
        const int gh = glyphs[c].h;
        if (gw <= 0 || gh <= 0 || !glyphs[c].bmp) continue;
        const int ox = (int)out[c].x;
        const int oy = (int)out[c].y;
        for (int yy = 0; yy < gh; yy++) {
            memcpy(atlas + (size_t)(oy + yy) * (size_t)atlas_w + (size_t)ox,
                   glyphs[c].bmp + (size_t)yy * (size_t)gw,
                   (size_t)gw);
        }
    }

    FILE *outf = fopen(out_path, "wb");
    if (!outf) die("cannot open out");

    fprintf(outf, "#include \"kernel.h\"\n");
    fprintf(outf, "#include \"font_atlas.h\"\n\n");
    fprintf(outf, "static const u8 g_font_atlas_alpha_");
    print_ident(outf, name);
    fprintf(outf, "[%zu] = {", (size_t)atlas_w * (size_t)atlas_h);
    for (size_t i = 0; i < (size_t)atlas_w * (size_t)atlas_h; i++) {
        if ((i % 32) == 0) fprintf(outf, "\n ");
        fprintf(outf, "%u,", (unsigned)atlas[i]);
    }
    fprintf(outf, "\n};\n\n");

    fprintf(outf, "static const font_glyph_t g_font_glyphs_");
    print_ident(outf, name);
    fprintf(outf, "[256] = {\n");
    for (int c = 0; c < 256; c++) {
        fprintf(outf, "  /* 0x%02X */ { %u,%u,%u,%u,%d,%d,%u },\n",
                c,
                (unsigned)out[c].x, (unsigned)out[c].y, (unsigned)out[c].w, (unsigned)out[c].h,
                (int)out[c].xoff, (int)out[c].yoff, (unsigned)out[c].xadvance);
    }
    fprintf(outf, "};\n");

    fprintf(outf, "\nconst font_face_t g_font_face_");
    print_ident(outf, name);
    fprintf(outf, " = {\n");
    fprintf(outf, "  .atlas_w = %d,\n", atlas_w);
    fprintf(outf, "  .atlas_h = %d,\n", atlas_h);
    fprintf(outf, "  .atlas_alpha = g_font_atlas_alpha_"); print_ident(outf, name); fprintf(outf, ",\n");
    fprintf(outf, "  .glyphs = g_font_glyphs_"); print_ident(outf, name); fprintf(outf, ",\n");
    fprintf(outf, "  .line_height = %d,\n", line_h);
    fprintf(outf, "  .ascent = %d,\n", ascent);
    fprintf(outf, "  .cell_advance = %d,\n", max_adv);
    fprintf(outf, "};\n");

    fclose(outf);

    for (uint32_t c = 0x20; c <= 0xFF; c++) free(glyphs[c].bmp);
    free(atlas);

    FT_Done_Face(face);
    FT_Done_FreeType(lib);

    return 0;
}
