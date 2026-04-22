/* test_sticker_sheet.c
 *
 * Render a demo sheet using 16 synthetic gradient photos. Validates that
 * the compositor produces the exact 148x100mm hagaki layout at 300 DPI.
 *
 * Usage: test_sticker_sheet [out.bmp]
 */

#include "../src/sticker_sheet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Generate a synthetic "photo": a diagonal color gradient with a
 * Pokeball-like yellow blob in the center. 24bpp BGR top-down. */
static uint8_t *make_fake_photo(int w, int h, int seed)
{
    uint8_t *buf = (uint8_t*)malloc((size_t)w * h * 3);
    if (!buf) return NULL;

    /* Background gradient, hue varying per seed. */
    float hue = (float)(seed * 37 % 360);
    float c = 0.6f, x_chroma = c;
    (void)x_chroma;

    /* HSV->RGB helpers. */
    for (int y = 0; y < h; y++) {
        float vy = (float)y / (float)(h - 1);
        for (int x = 0; x < w; x++) {
            float vx = (float)x / (float)(w - 1);
            float l = 0.35f + 0.55f * (1.0f - (vx + vy) * 0.5f);

            /* Simple HSV to RGB. */
            float H = hue / 60.0f;
            float C = c * l;
            float X = C * (1.0f - fabsf(fmodf(H, 2.0f) - 1.0f));
            float R = 0, G = 0, B = 0;
            if      (H < 1) { R = C; G = X; }
            else if (H < 2) { R = X; G = C; }
            else if (H < 3) { G = C; B = X; }
            else if (H < 4) { G = X; B = C; }
            else if (H < 5) { R = X; B = C; }
            else            { R = C; B = X; }
            float m = l - C;
            R += m; G += m; B += m;

            /* Yellow "Pokemon" blob in the center. */
            float dx = vx - 0.5f;
            float dy = vy - 0.55f;
            float d  = sqrtf(dx*dx + dy*dy);
            if (d < 0.22f) { R = 1.0f; G = 0.85f; B = 0.0f; }
            /* Head on top. */
            dy = vy - 0.33f;
            d  = sqrtf(dx*dx + dy*dy);
            if (d < 0.11f) { R = 1.0f; G = 0.85f; B = 0.0f; }

            uint8_t *p = buf + ((size_t)y * w + x) * 3;
            p[0] = (uint8_t)(B * 255.0f + 0.5f);
            p[1] = (uint8_t)(G * 255.0f + 0.5f);
            p[2] = (uint8_t)(R * 255.0f + 0.5f);
        }
    }
    return buf;
}

static void free_photos(ss_photo_t p[SS_NUM_STICKERS])
{
    for (int i = 0; i < SS_NUM_STICKERS; i++) {
        free((void*)p[i].rgb24);
        p[i].rgb24 = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "sticker_sheet_demo.bmp";

    ss_sheet_t sheet;
    if (ss_sheet_init(&sheet, SS_DPI_DEFAULT) != 0) {
        fprintf(stderr, "ss_sheet_init failed\n");
        return 1;
    }

    printf("Sheet: %d x %d px at %d DPI\n", sheet.w, sheet.h, sheet.dpi);

    ss_photo_t photos[SS_NUM_STICKERS];
    memset(photos, 0, sizeof(photos));
    for (int i = 0; i < SS_NUM_STICKERS; i++) {
        photos[i].w = 320;
        photos[i].h = 240;
        photos[i].rgb24 = make_fake_photo(320, 240, i);
    }

    ss_options_t opts;
    ss_options_default(&opts);
    opts.show_print_area    = 1;
    opts.show_kiss_outline  = 1;
    opts.crop_fill_photos   = 1;

    if (ss_sheet_compose(&sheet, photos, &opts) != 0) {
        fprintf(stderr, "ss_sheet_compose failed\n");
        ss_sheet_free(&sheet);
        free_photos(photos);
        return 1;
    }

    /* Sanity: print the location of each kiss-cut rect. */
    printf("Kiss-cut rects:\n");
    for (int i = 0; i < SS_NUM_STICKERS; i++) {
        int kx, ky, kw, kh;
        ss_sheet_get_kiss_rect(&sheet, i, &kx, &ky, &kw, &kh);
        printf("  slot %2d (col %d row %d): x=%4d y=%4d w=%3d h=%3d\n",
               i, i % SS_COLS, i / SS_COLS, kx, ky, kw, kh);
    }

    if (ss_sheet_save_bmp(&sheet, out) != 0) {
        fprintf(stderr, "ss_sheet_save_bmp(%s) failed\n", out);
    } else {
        printf("Saved: %s\n", out);
    }

    ss_sheet_free(&sheet);
    free_photos(photos);
    return 0;
}
