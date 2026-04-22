/* sticker_sheet.c
 *
 * Implementation of the Snap Station sticker-sheet compositor.
 * Pure C99, no deps beyond stdio/stdlib/math/string.
 */

#include "sticker_sheet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ----------------------------------------------------------------------- */
/* Public helpers                                                          */
/* ----------------------------------------------------------------------- */

void ss_options_default(ss_options_t *o)
{
    if (!o) return;
    o->show_kiss_outline   = 0;
    o->show_backing_guide  = 0;
    o->show_print_area     = 0;
    o->fill_outside_print  = 0;
    o->crop_fill_photos    = 1;
}

int ss_sheet_init(ss_sheet_t *s, int dpi)
{
    if (!s) return -1;
    if (dpi <= 0) dpi = SS_DPI_DEFAULT;
    s->dpi = dpi;
    s->w = SS_MM_TO_PX(SS_PAPER_W_MM, dpi);
    s->h = SS_MM_TO_PX(SS_PAPER_H_MM, dpi);
    s->pixels = (uint8_t*)malloc((size_t)s->w * s->h * 3);
    if (!s->pixels) return -1;
    memset(s->pixels, 0xFF, (size_t)s->w * s->h * 3);   /* white background */
    return 0;
}

void ss_sheet_free(ss_sheet_t *s)
{
    if (!s) return;
    free(s->pixels);
    s->pixels = NULL;
}

/* ----------------------------------------------------------------------- */
/* Geometry                                                                */
/* ----------------------------------------------------------------------- */

/* Compute the top-left pixel of the print area (centered on the sheet). */
static void print_origin(const ss_sheet_t *s, int *ox, int *oy)
{
    int print_w = SS_MM_TO_PX(SS_PRINT_W_MM, s->dpi);
    int print_h = SS_MM_TO_PX(SS_PRINT_H_MM, s->dpi);
    *ox = (s->w - print_w) / 2;
    *oy = (s->h - print_h) / 2;
}

/* Compute the backing rect for slot (col, row). */
static void backing_rect(const ss_sheet_t *s, int col, int row,
                         int *x, int *y, int *w, int *h)
{
    int ox, oy;
    print_origin(s, &ox, &oy);
    int stk_w   = SS_MM_TO_PX(SS_STICKER_W_MM, s->dpi);
    int stk_h   = SS_MM_TO_PX(SS_STICKER_H_MM, s->dpi);
    int gutter  = SS_MM_TO_PX(SS_GUTTER_MM,    s->dpi);
    *x = ox + col * (stk_w + gutter);
    *y = oy + row * (stk_h + gutter);
    *w = stk_w;
    *h = stk_h;
}

/* Compute the kiss-cut rect for slot (col, row). */
static void kiss_rect(const ss_sheet_t *s, int col, int row,
                      int *x, int *y, int *w, int *h)
{
    int bx, by, bw, bh;
    backing_rect(s, col, row, &bx, &by, &bw, &bh);
    int inset_side = SS_MM_TO_PX(SS_INSET_SIDE_MM,   s->dpi);
    int inset_top  = SS_MM_TO_PX(SS_INSET_TOP_MM,    s->dpi);
    int kiss_w     = SS_MM_TO_PX(SS_KISS_W_MM,       s->dpi);
    int kiss_h     = SS_MM_TO_PX(SS_KISS_H_MM,       s->dpi);
    *x = bx + inset_side;
    *y = by + inset_top;
    *w = kiss_w;
    *h = kiss_h;
}

void ss_sheet_get_kiss_rect(const ss_sheet_t *s, int slot_idx,
                            int *ox, int *oy, int *ow, int *oh)
{
    int col = slot_idx % SS_COLS;
    int row = slot_idx / SS_COLS;
    int x, y, w, h;
    kiss_rect(s, col, row, &x, &y, &w, &h);
    if (ox) *ox = x;
    if (oy) *oy = y;
    if (ow) *ow = w;
    if (oh) *oh = h;
}

/* Is (x, y) inside a rounded rect with radius r?
 * Rect occupies [0, w) x [0, h). Returns 1 inside, 0 outside. */
static int inside_rounded_rect(int x, int y, int w, int h, int r)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    if (r <= 0) return 1;
    int dx = -1, dy = -1;
    if      (x < r)          dx = r - 1 - x;
    else if (x >= w - r)     dx = x - (w - r);
    if      (y < r)          dy = r - 1 - y;
    else if (y >= h - r)     dy = y - (h - r);
    if (dx < 0 || dy < 0) return 1;             /* edge strips: always in */
    return (dx * dx + dy * dy) <= (r * r);      /* corner: distance test  */
}

/* ----------------------------------------------------------------------- */
/* Pixel blitting                                                          */
/* ----------------------------------------------------------------------- */

static uint8_t *px(uint8_t *buf, int stride, int x, int y)
{
    return buf + (size_t)y * stride + (size_t)x * 3;
}

static void put_px(uint8_t *buf, int w, int h, int x, int y,
                   uint8_t b, uint8_t g, uint8_t r)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t *p = px(buf, w * 3, x, y);
    p[0] = b; p[1] = g; p[2] = r;
}

/* Nearest-neighbour scale + crop-fill blit into a rounded-rect mask.
 * dst is the full sheet; (kx,ky,kw,kh) is the kiss-cut rect. */
static void blit_photo_rounded(uint8_t *dst, int dst_w, int dst_h,
                               int kx, int ky, int kw, int kh, int r,
                               const uint8_t *src, int sw, int sh,
                               int crop_fill)
{
    /* Compute source rect that maps to the destination rect. */
    float scale_x = (float)kw / (float)sw;
    float scale_y = (float)kh / (float)sh;
    float scale   = crop_fill ? (scale_x > scale_y ? scale_x : scale_y)
                              : (scale_x < scale_y ? scale_x : scale_y);
    int draw_w    = (int)(sw * scale + 0.5f);
    int draw_h    = (int)(sh * scale + 0.5f);
    int off_x     = kx + (kw - draw_w) / 2;
    int off_y     = ky + (kh - draw_h) / 2;

    /* Fill kiss rect with white first (so letterbox bars are white, and
     * outside-rounded-corner areas stay white). */
    for (int y = 0; y < kh; y++) {
        for (int x = 0; x < kw; x++) {
            if (inside_rounded_rect(x, y, kw, kh, r)) {
                put_px(dst, dst_w, dst_h, kx + x, ky + y, 0xFF, 0xFF, 0xFF);
            }
        }
    }

    /* Sample source nearest-neighbour into the kiss-cut mask. */
    for (int y = 0; y < draw_h; y++) {
        int dy = off_y + y;
        if (dy < ky || dy >= ky + kh) continue;
        int sy = (int)((float)y / scale);
        if (sy < 0 || sy >= sh) continue;
        for (int x = 0; x < draw_w; x++) {
            int dx = off_x + x;
            if (dx < kx || dx >= kx + kw) continue;
            if (!inside_rounded_rect(dx - kx, dy - ky, kw, kh, r)) continue;
            int sx = (int)((float)x / scale);
            if (sx < 0 || sx >= sw) continue;
            const uint8_t *sp = src + ((size_t)sy * sw + sx) * 3;
            uint8_t *dp = px(dst, dst_w * 3, dx, dy);
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
        }
    }
}

/* Placeholder checkerboard for missing slots. */
static void draw_placeholder(uint8_t *dst, int dst_w, int dst_h,
                             int kx, int ky, int kw, int kh, int r,
                             int slot_idx)
{
    const uint8_t a[3] = { 0xDC, 0xDC, 0xDC };   /* light gray  */
    const uint8_t b[3] = { 0xA0, 0xA0, 0xA0 };   /* medium gray */
    int cell = (kw < 32) ? 8 : 16;
    for (int y = 0; y < kh; y++) {
        for (int x = 0; x < kw; x++) {
            if (!inside_rounded_rect(x, y, kw, kh, r)) continue;
            int cx = x / cell, cy = y / cell;
            const uint8_t *c = ((cx + cy) & 1) ? b : a;
            put_px(dst, dst_w, dst_h, kx + x, ky + y, c[0], c[1], c[2]);
        }
    }
    /* Number the slot in the top-left corner. */
    char label[8];
    snprintf(label, sizeof(label), "%02d", slot_idx + 1);
    /* Small 3x5 debug glyphs. Only digits 0-9. */
    static const uint16_t glyphs[10] = {
        /* 3 cols x 5 rows, bits read col-major top-to-bottom, col 0 first */
        0x7B6Fu, 0x4924u, 0x73E7u, 0x73CFu, 0x5BC9u,
        0x79CFu, 0x79EFu, 0x7249u, 0x7BEFu, 0x7BCFu
    };
    int gy = 2, gx = 2;
    for (const char *p = label; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        uint16_t bits = glyphs[*p - '0'];
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if (bits & (1 << (row * 3 + col))) {
                    put_px(dst, dst_w, dst_h,
                           kx + gx + col, ky + gy + row, 0, 0, 0);
                }
            }
        }
        gx += 4;
    }
}

/* Draw a dashed rectangle outline. */
static void draw_dashed_rect(uint8_t *dst, int dst_w, int dst_h,
                             int x, int y, int w, int h, int dash_px,
                             uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < w; i++) {
        if (((i / dash_px) & 1) == 0) {
            put_px(dst, dst_w, dst_h, x + i, y,         b, g, r);
            put_px(dst, dst_w, dst_h, x + i, y + h - 1, b, g, r);
        }
    }
    for (int i = 0; i < h; i++) {
        if (((i / dash_px) & 1) == 0) {
            put_px(dst, dst_w, dst_h, x,         y + i, b, g, r);
            put_px(dst, dst_w, dst_h, x + w - 1, y + i, b, g, r);
        }
    }
}

/* Draw a rounded-rect outline (1 px stroke). */
static void draw_rounded_outline(uint8_t *dst, int dst_w, int dst_h,
                                 int kx, int ky, int kw, int kh, int r,
                                 uint8_t cr, uint8_t cg, uint8_t cb)
{
    for (int y = 0; y < kh; y++) {
        for (int x = 0; x < kw; x++) {
            int inside = inside_rounded_rect(x,   y,   kw, kh, r);
            int n_in   = inside_rounded_rect(x-1, y,   kw, kh, r) &&
                         inside_rounded_rect(x+1, y,   kw, kh, r) &&
                         inside_rounded_rect(x,   y-1, kw, kh, r) &&
                         inside_rounded_rect(x,   y+1, kw, kh, r);
            if (inside && !n_in) {
                put_px(dst, dst_w, dst_h, kx + x, ky + y, cb, cg, cr);
            }
        }
    }
}

/* ----------------------------------------------------------------------- */
/* Composition                                                             */
/* ----------------------------------------------------------------------- */

int ss_sheet_compose(ss_sheet_t *s, const ss_photo_t photos[SS_NUM_STICKERS],
                     const ss_options_t *opts)
{
    if (!s || !s->pixels || !photos) return -1;

    ss_options_t defaults;
    if (!opts) { ss_options_default(&defaults); opts = &defaults; }

    int r = SS_MM_TO_PX(SS_CORNER_R_MM, s->dpi);

    /* Optionally tint non-print area very light gray. */
    if (opts->fill_outside_print) {
        int ox, oy;
        int pw = SS_MM_TO_PX(SS_PRINT_W_MM, s->dpi);
        int ph = SS_MM_TO_PX(SS_PRINT_H_MM, s->dpi);
        print_origin(s, &ox, &oy);
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++)
                if (x < ox || y < oy || x >= ox + pw || y >= oy + ph)
                    put_px(s->pixels, s->w, s->h, x, y, 0xF4, 0xF4, 0xF4);
    }

    if (opts->show_print_area) {
        int ox, oy;
        print_origin(s, &ox, &oy);
        int pw = SS_MM_TO_PX(SS_PRINT_W_MM, s->dpi);
        int ph = SS_MM_TO_PX(SS_PRINT_H_MM, s->dpi);
        draw_dashed_rect(s->pixels, s->w, s->h, ox, oy, pw, ph, 8,
                         0xFF, 0x8C, 0x00);
    }

    for (int i = 0; i < SS_NUM_STICKERS; i++) {
        int col = i % SS_COLS, row = i / SS_COLS;
        int bx, by, bw, bh, kx, ky, kw, kh;
        backing_rect(s, col, row, &bx, &by, &bw, &bh);
        kiss_rect   (s, col, row, &kx, &ky, &kw, &kh);

        const ss_photo_t *ph = &photos[i];
        if (ph->rgb24 && ph->w > 0 && ph->h > 0) {
            blit_photo_rounded(s->pixels, s->w, s->h,
                               kx, ky, kw, kh, r,
                               ph->rgb24, ph->w, ph->h,
                               opts->crop_fill_photos);
        } else {
            draw_placeholder(s->pixels, s->w, s->h,
                             kx, ky, kw, kh, r, i);
        }

        if (opts->show_backing_guide)
            draw_dashed_rect(s->pixels, s->w, s->h, bx, by, bw, bh, 4,
                             0xC0, 0xC0, 0xC0);
        if (opts->show_kiss_outline)
            draw_rounded_outline(s->pixels, s->w, s->h, kx, ky, kw, kh, r,
                                 0x20, 0x20, 0x20);
    }

    return 0;
}

/* ----------------------------------------------------------------------- */
/* BMP writer                                                              */
/* ----------------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} ss_bmp_fh;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} ss_bmp_ih;
#pragma pack(pop)

int ss_sheet_save_bmp(const ss_sheet_t *s, const char *path)
{
    if (!s || !s->pixels || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int src_stride = s->w * 3;
    int row_stride = (src_stride + 3) & ~3;
    uint32_t pix_bytes = (uint32_t)row_stride * (uint32_t)s->h;

    /* DPI -> pixels per meter (for BMP metadata). */
    int32_t ppm = (int32_t)((float)s->dpi / 0.0254f + 0.5f);

    ss_bmp_fh fh;
    ss_bmp_ih ih;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + pix_bytes;
    ih.biSize        = sizeof(ih);
    ih.biWidth       = s->w;
    ih.biHeight      = -s->h;                  /* top-down */
    ih.biPlanes      = 1;
    ih.biBitCount    = 24;
    ih.biCompression = 0;
    ih.biSizeImage   = pix_bytes;
    ih.biXPelsPerMeter = ppm;
    ih.biYPelsPerMeter = ppm;

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);

    if (row_stride == src_stride) {
        fwrite(s->pixels, pix_bytes, 1, f);
    } else {
        uint8_t pad[3] = { 0, 0, 0 };
        for (int y = 0; y < s->h; y++) {
            fwrite(s->pixels + (size_t)y * src_stride, src_stride, 1, f);
            fwrite(pad, row_stride - src_stride, 1, f);
        }
    }

    fclose(f);
    return 0;
}
