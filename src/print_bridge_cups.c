/* print_bridge_cups.c
 *
 * CUPS-backed print bridge for Linux and macOS. Writes the composed
 * sticker sheet to a temp file and invokes `lp` via posix_spawn. No
 * CUPS-library linkage required so this compiles in CI images that
 * only ship `cups-client`.
 *
 * Windows implementation lives in src/print_bridge_win32.c (pending).
 */
#include "print_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static bool write_temp(const void *data, size_t len, char *path, size_t path_sz) {
    snprintf(path, path_sz, "/tmp/snapstation_XXXXXX.bmp");
    int fd = mkstemps(path, 4);
    if (fd < 0) return false;
    ssize_t n = write(fd, data, len);
    close(fd);
    return n == (ssize_t)len;
}

bool print_bridge_submit(const void *image, size_t image_len,
                         const print_bridge_opts_t *opts) {
    if (!image || !image_len || !opts) return false;
    char path[64];
    if (!write_temp(image, image_len, path, sizeof(path))) return false;

    pid_t pid = fork();
    if (pid < 0) { unlink(path); return false; }
    if (pid == 0) {
        if (opts->printer_name && opts->media) {
            execlp("lp", "lp", "-d", opts->printer_name,
                   "-o", opts->media, path, (char *)NULL);
        } else if (opts->printer_name) {
            execlp("lp", "lp", "-d", opts->printer_name, path, (char *)NULL);
        } else {
            execlp("lp", "lp", path, (char *)NULL);
        }
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(path);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
