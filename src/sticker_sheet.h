/* sticker_sheet.h
 *
 * Composite a 4x4 Snap Station sticker sheet from 16 captured N64 frames.
 * Dimensions are taken directly from Andrew's 2023 measurements of a real
 * retail sticker sheet (CC0, Internet Archive item slide-1_202310).
 *
 * Paper           148.0 x 100.0 mm (Japanese hagaki postcard)
 * Print area      109.4 x  83.0 mm
 * Each cell       26.6 x 20.0 mm (backing)
 * Kiss-cut        24.1 x 17.5 mm inside each backing
 * Corner radius   2.75 mm
 * Insets          0.833 top / 1.667 bottom / 1.25 side (mm)
 * Gutters         1.0 mm between cells, horizontal and vertical
 *
 * All rendering is done in 24-bit BGR (DIB-native) at 300 DPI by default,
 * producing a 1748x1181-pixel sheet ready for the Windows print dialog.
 */

#ifndef STICKER_SHEET_H
#define STICKER_SHEET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Physical constants (millimeters). */
#define SS_PAPER_W_MM          148.0f
#define SS_PAPER_H_MM          100.0f
#define SS_PRINT_W_MM          109.4f
#define SS_PRINT_H_MM           83.0f
#define SS_STICKER_W_MM         26.6f
#define SS_STICKER_H_MM         20.0f
#define SS_KISS_W_MM            24.1f
#define SS_KISS_H_MM            17.5f
#define SS_CORNER_R_MM           2.75f
#define SS_INSET_TOP_MM          0.833f
#define SS_INSET_BOTTOM_MM       1.667f
#define SS_INSET_SIDE_MM         1.25f
#define SS_GUTTER_MM             1.0f
#define SS_COLS                  4
#define SS_ROWS                  4
#define SS_NUM_STICKERS          16      /* SS_COLS * SS_ROWS */

/* Default print resolution (dots per inch). */
#define SS_DPI_DEFAULT           300

/* mm->px helper. */
#define SS_MM_TO_PX(mm, dpi)    ((int)(((float)(mm) * (float)(dpi) / 25.4f) + 0.5f))

/* One captured N64 frame. rgb24 is tightly packed 24bpp BGR (DIB-style),
 * top-down, size = w*h*3 bytes. If rgb24 is NULL the slot renders as a
 * checkerboard placeholder. */
typedef struct {
    const uint8_t *rgb24;
    int            w;
    int            h;
} ss_photo_t;

/* Full sheet descriptor. The caller owns 'pixels'. */
typedef struct {
    uint8_t *pixels;         /* 24bpp BGR, top-down, size = w*h*3 */
    int      w;
    int      h;
    int      dpi;
} ss_sheet_t;

/* Rendering options. */
typedef struct {
    int      show_kiss_outline;   /* draw dashed line around each kiss-cut */
    int      show_backing_guide;  /* draw dashed line around each backing */
    int      show_print_area;     /* draw dashed line around 109.4 x 83 */
    int      fill_outside_print;  /* fill non-print area with light gray   */
    int      crop_fill_photos;    /* 1 = crop-fill, 0 = letterbox inside   */
} ss_options_t;

/* Fill an ss_options_t with defaults appropriate for an actual print run
 * (no guides, crop-fill). */
void ss_options_default(ss_options_t *opts);

/* Allocate a blank sheet at the given DPI. Returns 0 on success. */
int  ss_sheet_init(ss_sheet_t *sheet, int dpi);

/* Free sheet pixels. */
void ss_sheet_free(ss_sheet_t *sheet);

/* Compose the sheet: drop 16 captured photos into the 16 slots. Slots
 * with photos[i].rgb24 == NULL get a placeholder. Returns 0 on success. */
int  ss_sheet_compose(ss_sheet_t *sheet, const ss_photo_t photos[SS_NUM_STICKERS],
                      const ss_options_t *opts);

/* Save the sheet as a 24bpp BMP. Returns 0 on success. */
int  ss_sheet_save_bmp(const ss_sheet_t *sheet, const char *path);

/* Get the pixel rectangle of the kiss-cut for slot index (0..15). Useful
 * for tests and for the Win32 printer dialog code that needs to know
 * where to place overlays. */
void ss_sheet_get_kiss_rect(const ss_sheet_t *sheet, int slot_idx,
                            int *out_x, int *out_y,
                            int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* STICKER_SHEET_H */
