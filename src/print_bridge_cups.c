/* print_bridge_cups.c
 *
 * CUPS-backed print bridge for Linux and macOS. Uses posix_spawnp so
 * the print call is safe from the multi-threaded Mupen64Plus plugin
 * process (fork() in a threaded process can deadlock on some libc
 * implementations).
 *
 * Windows implementation lives in src/print_bridge_win32.c (pending).
 */
#include "print_bridge.h"

#include <spawn.h>
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

bool print_bridge_submit_path(const char *path,
                              const print_bridge_opts_t *opts) {
    if (!path || !opts) return false;

    char *argv[8];
    int i = 0;
    argv[i++] = (char *)"lp";
    if (opts->printer_name) {
        argv[i++] = (char *)"-d";
        argv[i++] = (char *)opts->printer_name;
    }
    if (opts->media) {
        argv[i++] = (char *)"-o";
        argv[i++] = (char *)opts->media;
    }
    argv[i++] = (char *)path;
    argv[i] = NULL;

    pid_t pid = 0;
    if (posix_spawnp(&pid, "lp", NULL, NULL, argv, environ) != 0) {
        return false;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
