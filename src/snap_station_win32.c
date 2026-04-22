/* snap_station_win32.c
 *
 * Windows bridge between the JoyBus state machine, the Mupen64Plus
 * screenshot API, the sticker-sheet compositor, and the Windows print
 * dialog.
 *
 * Responsibilities:
 *   - On each 0x02 (photo ready) event, invoke Mupen64Plus's
 *     M64CMD_TAKE_NEXT_SCREENSHOT so the video plugin writes a PNG.
 *   - Track the sequence of captured PNGs (by scanning the screenshot
 *     directory modification times).
 *   - On 0x04 (end of display) event, load the 16 most recent PNGs,
 *     composite them via sticker_sheet, save a BMP of the resulting
 *     148x100 mm hagaki sheet, and present the Windows print dialog.
 *   - Dump the captured job metadata to %USERPROFILE%\Documents\SnapStation.
 *
 * Link against user32.lib, gdi32.lib, comdlg32.lib, winspool.lib,
 * shell32.lib.
 */

#ifdef _WIN32

#include "sticker_sheet.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <winspool.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* stb_image (public domain, github.com/nothings/stb) decodes the PNG
 * screenshots that mupen64plus's video plugins emit. BMP kept enabled
 * so the same loader handles both formats. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#ifdef _MSC_VER
#  pragma comment(lib, "comdlg32.lib")
#  pragma comment(lib, "winspool.lib")
#  pragma comment(lib, "gdi32.lib")
#  pragma comment(lib, "user32.lib")
#  pragma comment(lib, "shell32.lib")
#endif

/* Mupen64Plus core API: we dynamically resolve CoreDoCommand from the
 * mupen64plus.dll module so we don't have to link-time bind. */
typedef int (*CoreDoCommand_t)(int Command, int ParamInt, void *ParamPtr);

/* Mupen64Plus m64p_types.h enum values. These are ZERO-indexed and the
 * enum order is load-bearing (changing it would break ABI). Off-by-one
 * here silently corrupts to an adjacent command:
 *   15 = M64CMD_SET_FRAME_CALLBACK     (takes a frame-index callback,
 *                                        not a screenshot)
 *   16 = M64CMD_TAKE_NEXT_SCREENSHOT  <-- what we want
 *   17 = M64CMD_CORE_STATE_SET
 *   19 = M64CMD_RESET                  (soft=0 / hard=1 as ParamInt) */
#define M64CMD_TAKE_NEXT_SCREENSHOT 16

static CoreDoCommand_t g_core_do_command = NULL;

static void resolve_core(void)
{
    if (g_core_do_command) return;
    HMODULE h = GetModuleHandleA("mupen64plus.dll");
    if (h) g_core_do_command = (CoreDoCommand_t)GetProcAddress(h, "CoreDoCommand");
}

/* Captured photo paths (queued by on_capture, loaded by on_print). */
static char       g_capture_paths[SS_NUM_STICKERS][MAX_PATH];
static int        g_capture_count;
static char       g_screenshot_dir[MAX_PATH];

/* Job baseline: the newest screenshot mtime seen in the directory at the
 * moment the current print job started. load_recent_captures() filters
 * to files strictly newer than this value so a half-completed or prior
 * session's PNGs can't leak into the current sheet. */
static FILETIME   g_job_baseline_ft;
static int        g_job_active;

/* Query Mupen64Plus's configured screenshot dir. Defaults to
 * %USERPROFILE%\AppData\Roaming\Mupen64Plus\screenshot\ if we can't find
 * the exact value. */
static void discover_screenshot_dir(void)
{
    if (g_screenshot_dir[0]) return;
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL,
                                   SHGFP_TYPE_CURRENT, appdata))) {
        snprintf(g_screenshot_dir, sizeof(g_screenshot_dir),
                 "%s\\Mupen64Plus\\screenshot", appdata);
    } else {
        strcpy(g_screenshot_dir, ".\\screenshot");
    }
}

static void get_output_dir(char *out, size_t outsz)
{
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL,
                                   SHGFP_TYPE_CURRENT, path))) {
        snprintf(out, outsz, "%s\\SnapStation", path);
    } else {
        snprintf(out, outsz, ".\\SnapStation");
    }
    CreateDirectoryA(out, NULL);
}

/* Load any stb-supported image (PNG or BMP) and convert to tightly-
 * packed 24bpp BGR (DIB-native), top-down, matching ss_photo_t's input
 * contract (see sticker_sheet.h). Returns NULL on failure. Caller frees
 * via free() - stb uses the system allocator by default. */
static uint8_t *load_image_bgr24(const char *path, int *out_w, int *out_h)
{
    int w = 0, h = 0, ch = 0;
    uint8_t *rgb = stbi_load(path, &w, &h, &ch, 3);
    if (!rgb) return NULL;

    size_t px = (size_t)w * (size_t)h;
    for (size_t i = 0; i < px; i++) {
        uint8_t r = rgb[i * 3 + 0];
        rgb[i * 3 + 0] = rgb[i * 3 + 2];
        rgb[i * 3 + 2] = r;
    }
    *out_w = w; *out_h = h;
    return rgb;
}

/* Print a 24bpp BGR image to hdcPrinter, fit to page. */
static BOOL print_image(HDC hdcPrinter, int w, int h, const uint8_t *bgr24)
{
    DOCINFOA di; memset(&di, 0, sizeof(di)); di.cbSize = sizeof(di);
    di.lpszDocName = "Pokemon Snap Station sticker sheet";
    if (StartDocA(hdcPrinter, &di) <= 0) return FALSE;
    if (StartPage(hdcPrinter) <= 0) { EndDoc(hdcPrinter); return FALSE; }

    int pageW = GetDeviceCaps(hdcPrinter, HORZRES);
    int pageH = GetDeviceCaps(hdcPrinter, VERTRES);
    float sx = (float)pageW / (float)w;
    float sy = (float)pageH / (float)h;
    float s  = sx < sy ? sx : sy;
    int drawW = (int)(w * s);
    int drawH = (int)(h * s);
    int offX  = (pageW - drawW) / 2;
    int offY  = (pageH - drawH) / 2;

    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    int src_stride = w * 3;
    int dib_stride = (src_stride + 3) & ~3;
    uint8_t *dib = (uint8_t*)calloc((size_t)dib_stride * h, 1);
    for (int y = 0; y < h; y++)
        memcpy(dib + y * dib_stride, bgr24 + y * src_stride, src_stride);

    SetStretchBltMode(hdcPrinter, HALFTONE);
    StretchDIBits(hdcPrinter, offX, offY, drawW, drawH,
                  0, 0, w, h, dib, &bi, DIB_RGB_COLORS, SRCCOPY);

    free(dib);
    EndPage(hdcPrinter);
    EndDoc(hdcPrinter);
    return TRUE;
}

/* ================================================================== */
/* Bridge entry points (called from m64p_input_plugin.c)               */
/* ================================================================== */

void m64p_ss_take_screenshot(int photo_idx)
{
    resolve_core();
    discover_screenshot_dir();

    if (photo_idx < 0 || photo_idx >= SS_NUM_STICKERS) return;

    if (g_core_do_command) {
        g_core_do_command(M64CMD_TAKE_NEXT_SCREENSHOT, 0, NULL);
    } else {
        fprintf(stderr, "[snap_station] core not resolved - screenshot skipped\n");
    }

    /* Remember: we don't know the exact PNG filename the video plugin
     * will write (it's based on ROM title + counter). On m64p_ss_compose_and_print
     * we sort the screenshot dir by mtime and take the 16 most recent. */
    g_capture_paths[photo_idx][0] = 0;
    g_capture_count = photo_idx + 1;
}

/* ================================================================== */
/* Vision-aided RAM search: take a screenshot on demand and classify a
 * rectangular ROI of the latest PNG by mean HSV saturation. The search
 * loop in m64p_input_plugin.c uses this to tell gray-disabled buttons
 * apart from saturated enabled ones.                                   */
/* ================================================================== */

/* Fire CoreDoCommand(M64CMD_TAKE_NEXT_SCREENSHOT). The PNG is written
 * asynchronously by the video plugin; the caller must poll
 * m64p_ss_vision_measure_roi until a new file materialises. Returns 0
 * on success, -1 on failure. */
int m64p_ss_vision_snap(void)
{
    resolve_core();
    discover_screenshot_dir();
    if (!g_core_do_command) return -1;
    return g_core_do_command(M64CMD_TAKE_NEXT_SCREENSHOT, 0, NULL);
}

/* Pick the newest *.png in the screenshot dir with mtime > *baseline_io.
 * If found, load it via stb_image, compute the mean HSV saturation of
 * the ROI defined by (x0,y0,x1,y1) as percentages of width/height, and
 * return it in [0.0, 1.0]. On success *baseline_io is updated to the
 * found file's mtime so repeat calls advance past it. Returns -1.0 if
 * no new PNG has landed yet. */
float m64p_ss_vision_measure_roi(FILETIME *baseline_io,
                                 float x0_pct, float y0_pct,
                                 float x1_pct, float y1_pct)
{
    discover_screenshot_dir();

    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.png", g_screenshot_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1.0f;

    char     newest_name[MAX_PATH] = {0};
    FILETIME newest_ft = *baseline_io;
    int      found = 0;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &newest_ft) > 0) {
            newest_ft = fd.ftLastWriteTime;
            snprintf(newest_name, sizeof(newest_name), "%s\\%s",
                     g_screenshot_dir, fd.cFileName);
            found = 1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (!found) return -1.0f;

    int w = 0, h2 = 0, ch = 0;
    uint8_t *rgb = stbi_load(newest_name, &w, &h2, &ch, 3);
    if (!rgb) return -1.0f;

    int x0 = (int)(x0_pct * w);
    int y0 = (int)(y0_pct * h2);
    int x1 = (int)(x1_pct * w);
    int y1 = (int)(y1_pct * h2);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w; if (y1 > h2) y1 = h2;
    if (x1 <= x0 || y1 <= y0) { stbi_image_free(rgb); return -1.0f; }

    double s_sum = 0.0;
    int    s_n   = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            int R = p[0], G = p[1], B = p[2];
            int mx = R > G ? (R > B ? R : B) : (G > B ? G : B);
            int mn = R < G ? (R < B ? R : B) : (G < B ? G : B);
            float s = (mx == 0) ? 0.0f : (float)(mx - mn) / (float)mx;
            s_sum += s;
            s_n++;
        }
    }
    stbi_image_free(rgb);
    if (!s_n) return -1.0f;

    *baseline_io = newest_ft;
    return (float)(s_sum / s_n);
}

void m64p_ss_request_reset(void)
{
    /* Soft-reset the emulator so the Snap Station ROM transitions into
     * photo display mode. Mupen64Plus exposes this via:
     *   CoreDoCommand(M64CMD_RESET, 0 (soft), NULL);
     * We leave it opt-in because some ROMs behave better with a manual
     * reset; fire it only if the core hook is available. */
    resolve_core();
    if (g_core_do_command) {
        /* M64CMD_RESET = 19 per m64p_types.h. The old value 6 here was
         * M64CMD_STOP - which would quit the emulator instead of
         * soft-resetting the ROM. ParamInt: 0 = soft, 1 = hard. */
        #define M64CMD_RESET 19
        g_core_do_command(M64CMD_RESET, 0, NULL);
    }
}

/* Load up to `count` PNG screenshots whose mtime is strictly newer than
 * `baseline`, sorted ascending by mtime so photo slot 0 matches the
 * first 0x02 event of the current job. Returns number loaded; caller
 * must free(out[i].rgb) for each loaded slot.
 *
 * Filtering by baseline is how we keep a prior session's screenshots (or
 * unrelated PNGs that happen to be in the screenshot dir) out of this
 * sheet. Without it, load_recent_captures would blindly grab the 16
 * most-recent-overall PNGs, which may or may not belong to this job. */
typedef struct { uint8_t *rgb; int w; int h; } cap_t;

static int count_new_pngs(const char *dir, const FILETIME *baseline)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.png", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, baseline) > 0) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static int load_recent_captures(const char *dir, int count, cap_t *out,
                                const FILETIME *baseline)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.png", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    /* Collect only files strictly newer than baseline. */
    struct entry { FILETIME t; char name[MAX_PATH]; } cands[64];
    int n = 0;
    do {
        if (n >= 64) break;
        if (CompareFileTime(&fd.ftLastWriteTime, baseline) <= 0) continue;
        cands[n].t = fd.ftLastWriteTime;
        snprintf(cands[n].name, sizeof(cands[n].name), "%s\\%s",
                 dir, fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    /* Sort ascending by mtime so photo slot 0 is the first new capture. */
    for (int i = 1; i < n; i++) {
        struct entry e = cands[i];
        int j = i - 1;
        while (j >= 0 && CompareFileTime(&cands[j].t, &e.t) > 0) {
            cands[j+1] = cands[j]; j--;
        }
        cands[j+1] = e;
    }

    int taken = n < count ? n : count;
    for (int i = 0; i < taken; i++) {
        out[i].rgb = load_image_bgr24(cands[i].name, &out[i].w, &out[i].h);
        if (!out[i].rgb) {
            out[i].w = out[i].h = 0;
        }
    }
    return taken;
}

/* Start a new print job. Records the current newest-PNG mtime in the
 * screenshot dir as the baseline so we can later distinguish "new"
 * screenshots written during this job from pre-existing ones. Called
 * from m64p_input_plugin.c at the start of a DISPLAY_BEGIN/0x01 flow. */
void m64p_ss_begin_job(void)
{
    discover_screenshot_dir();
    memset(&g_job_baseline_ft, 0, sizeof(g_job_baseline_ft));

    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.png", g_screenshot_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (CompareFileTime(&fd.ftLastWriteTime, &g_job_baseline_ft) > 0)
                g_job_baseline_ft = fd.ftLastWriteTime;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    g_capture_count = 0;
    g_job_active = 1;
    for (int i = 0; i < SS_NUM_STICKERS; i++) g_capture_paths[i][0] = 0;

    fprintf(stderr,
        "[snap_station] begin job: baseline FT hi=%lu lo=%lu in %s\n",
        (unsigned long)g_job_baseline_ft.dwHighDateTime,
        (unsigned long)g_job_baseline_ft.dwLowDateTime,
        g_screenshot_dir);
}

/* Compose the sheet and run the print dialog. Returns 0 only when a
 * print job was actually accepted (PrintDlg returned a DC and print_image
 * succeeded). Any failure - too few new captures, sheet init failure,
 * user cancels, PrintDlg cancel, StartDoc/EndDoc errors - returns
 * non-zero so the caller can keep credits intact. */
int m64p_ss_compose_and_print(void)
{
    discover_screenshot_dir();

    if (!g_job_active) {
        fprintf(stderr,
            "[snap_station] compose: no active job (begin_job not called); "
            "refusing to print\n");
        return -1;
    }

    /* Screenshots are written asynchronously by the video plugin. By the
     * time 0x04 fires we expect all 16 to be on disk, but on a slow
     * machine the last few may still be flushing. Poll for up to ~2s
     * before giving up. */
    int new_png_count = 0;
    for (int attempt = 0; attempt < 20; attempt++) {
        new_png_count = count_new_pngs(g_screenshot_dir, &g_job_baseline_ft);
        if (new_png_count >= SS_NUM_STICKERS) break;
        Sleep(100);
    }
    if (new_png_count < SS_NUM_STICKERS) {
        fprintf(stderr,
            "[snap_station] compose: only %d/%d new captures after wait - "
            "aborting print (no credit consumed)\n",
            new_png_count, SS_NUM_STICKERS);
        g_job_active = 0;
        g_capture_count = 0;
        return -1;
    }

    char out_dir[MAX_PATH], ts[32];
    get_output_dir(out_dir, sizeof(out_dir));

    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", lt);

    ss_sheet_t sheet;
    if (ss_sheet_init(&sheet, SS_DPI_DEFAULT) != 0) {
        fprintf(stderr, "[snap_station] compose: sheet init failed\n");
        g_job_active = 0;
        g_capture_count = 0;
        return -1;
    }

    cap_t caps[SS_NUM_STICKERS];
    memset(caps, 0, sizeof(caps));
    int loaded = load_recent_captures(g_screenshot_dir, SS_NUM_STICKERS,
                                      caps, &g_job_baseline_ft);
    fprintf(stderr, "[snap_station] compose: loaded %d/%d new screenshots from %s\n",
            loaded, SS_NUM_STICKERS, g_screenshot_dir);

    int rc = -1;
    if (loaded < SS_NUM_STICKERS) {
        fprintf(stderr,
            "[snap_station] compose: short load - aborting print\n");
        goto cleanup;
    }

    ss_photo_t photos[SS_NUM_STICKERS];
    for (int i = 0; i < SS_NUM_STICKERS; i++) {
        photos[i].rgb24 = caps[i].rgb;
        photos[i].w     = caps[i].w;
        photos[i].h     = caps[i].h;
    }

    ss_options_t opts;
    ss_options_default(&opts);
    opts.crop_fill_photos = 1;
    ss_sheet_compose(&sheet, photos, &opts);

    char bmp_path[MAX_PATH];
    snprintf(bmp_path, sizeof(bmp_path), "%s\\sheet_%s.bmp", out_dir, ts);
    ss_sheet_save_bmp(&sheet, bmp_path);

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "Snap Station assembled a sticker sheet.\n\n"
             "  Sheet: %d x %d px @ %d DPI\n"
             "  Timestamp: %s\n"
             "  Saved: %s\n\n"
             "Press OK to choose a printer. Cancel leaves your credit\n"
             "balance unchanged.",
             sheet.w, sheet.h, sheet.dpi, ts, bmp_path);
    int r = MessageBoxA(NULL, msg, "Pokemon Snap Station",
                        MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
    if (r != IDOK) {
        fprintf(stderr, "[snap_station] compose: user cancelled pre-print\n");
        goto cleanup;
    }

    PRINTDLGA pd; memset(&pd, 0, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;
    BOOL dlg_ok = PrintDlgA(&pd);
    if (!dlg_ok || !pd.hDC) {
        /* PrintDlg returns 0 on cancel or failure; CommDlgExtendedError
         * returns 0 specifically for user cancel. Either way, no credit
         * should be charged. */
        fprintf(stderr,
            "[snap_station] compose: PrintDlg cancelled or failed "
            "(err=0x%lx)\n", (unsigned long)CommDlgExtendedError());
        if (pd.hDevMode)  GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        goto cleanup;
    }
    BOOL printed = print_image(pd.hDC, sheet.w, sheet.h, sheet.pixels);
    DeleteDC(pd.hDC);
    if (pd.hDevMode)  GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    if (!printed) {
        fprintf(stderr,
            "[snap_station] compose: StartDoc/EndDoc failed - no credit\n");
        goto cleanup;
    }
    rc = 0;

cleanup:
    for (int i = 0; i < SS_NUM_STICKERS; i++) free(caps[i].rgb);
    ss_sheet_free(&sheet);
    g_capture_count = 0;
    g_job_active = 0;
    return rc;
}

#endif /* _WIN32 */
