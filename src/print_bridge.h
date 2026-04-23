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
 */
#ifndef SNAP_STATION_PRINT_BRIDGE_H
#define SNAP_STATION_PRINT_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Null-terminated printer name as the OS reports it. NULL means
     * "use the system default printer." */
    const char *printer_name;
    /* Media size hint. For Snap Station output the only supported size
     * is hagaki (100x148 mm); other values are reserved. */
    const char *media;
    /* If true, skip the OS confirmation dialog. On Windows this drops
     * straight into the spooler; on Linux/macOS `lp` never dialogs. */
    bool silent;
} print_bridge_opts_t;

/* Submit a composed sticker sheet image to the print subsystem. `image`
 * is raw BMP or PNG bytes produced by src/sticker_sheet.c. Returns true
 * on successful submission (not successful print; the job is queued).
 */
bool print_bridge_submit(const void *image, size_t image_len,
                         const print_bridge_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif
