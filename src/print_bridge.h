/* print_bridge.h
 *
 * Platform-agnostic print bridge. Replaces snap_station_win32.c with a
 * cross-platform interface. Implementations:
 *   - src/print_bridge_cups.c   (Linux/macOS via CUPS `lp`)
 *   - src/print_bridge_win32.c  (Windows via the spooler; to follow)
 *
 * The existing snap_station_win32.c stays on main through Phase 2 so
 * Windows builds do not break mid-flight. When the win32 bridge lands,
 * it moves to archive/ with the .bat scripts.
 *
 * Callers produce the sticker sheet via src/sticker_sheet.c's
 * ss_sheet_save_bmp() and pass the resulting path here; this keeps the
 * BMP writer as the single source of truth for on-disk format.
 */
#ifndef SNAP_STATION_PRINT_BRIDGE_H
#define SNAP_STATION_PRINT_BRIDGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Null-terminated printer name as the OS reports it. NULL means
     * "use the system default printer." */
    const char *printer_name;
    /* Media size hint. For Snap Station output the only supported size
     * is hagaki (100x148 mm); other values are reserved. NULL defers
     * to printer defaults. */
    const char *media;
} print_bridge_opts_t;

/* Submit a composed sticker sheet file (BMP, as produced by
 * ss_sheet_save_bmp) to the print subsystem. Returns true on successful
 * submission (queued); actual print completion is asynchronous. */
bool print_bridge_submit_path(const char *path,
                              const print_bridge_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif
