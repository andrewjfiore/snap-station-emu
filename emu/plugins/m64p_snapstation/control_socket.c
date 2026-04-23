/* control_socket.c
 *
 * Loopback JSON control surface. Minimal hand-rolled parser so we do
 * not drag in a JSON library for one tiny protocol. See README.md for
 * the full command list.
 */
#include "control_socket.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define closesock closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <pthread.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define closesock close
#endif

static const char OK_REPLY[] = "{\"ok\":true}\n";

struct control_socket {
    sock_t listener;
    control_socket_callbacks_t cb;
    int running;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
};

static int find_number(const char *s, const char *key, long *out) {
    const char *p = strstr(s, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    *out = strtol(p + 1, NULL, 10);
    return 1;
}

/* Copy the string value of a JSON field into `out`. Returns false on
 * any malformed input. Fixes the strncmp-prefix trap of comparing
 * "press_print" against "press_printXYZ". */
static bool extract_string_field(const char *line, const char *key,
                                 char *out, size_t out_sz) {
    const char *p = strstr(line, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t n = (size_t)(end - p);
    if (n >= out_sz) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static void handle_line(struct control_socket *cs, const char *line,
                        char *reply, size_t reply_sz) {
    char cmd[32];
    if (!extract_string_field(line, "\"cmd\"", cmd, sizeof(cmd))) {
        snprintf(reply, reply_sz, "{\"error\":\"bad cmd\"}\n");
        return;
    }

    if (!strcmp(cmd, "insert_card") && cs->cb.insert_card) {
        long n = 0;
        find_number(line, "\"credits\"", &n);
        cs->cb.insert_card((uint32_t)n);
        snprintf(reply, reply_sz, "%s", OK_REPLY);
    } else if (!strcmp(cmd, "remove_card") && cs->cb.remove_card) {
        cs->cb.remove_card();
        snprintf(reply, reply_sz, "%s", OK_REPLY);
    } else if (!strcmp(cmd, "get_balance") && cs->cb.get_balance) {
        snprintf(reply, reply_sz, "{\"credits\":%u}\n",
                 (unsigned)cs->cb.get_balance());
    } else if (!strcmp(cmd, "press_print") && cs->cb.press_print) {
        cs->cb.press_print();
        snprintf(reply, reply_sz, "%s", OK_REPLY);
    } else if (!strcmp(cmd, "photo_ready") && cs->cb.photo_ready) {
        cs->cb.photo_ready();
        snprintf(reply, reply_sz, "%s", OK_REPLY);
    } else if (!strcmp(cmd, "end_print") && cs->cb.end_print) {
        cs->cb.end_print();
        snprintf(reply, reply_sz, "%s", OK_REPLY);
    } else if (!strcmp(cmd, "peek_flow") && cs->cb.peek_flow) {
        snprintf(reply, reply_sz, "{\"flow\":\"0x%02X\"}\n",
                 cs->cb.peek_flow());
    } else {
        snprintf(reply, reply_sz, "{\"error\":\"unknown cmd\"}\n");
    }
}

static void serve_client(struct control_socket *cs, sock_t client) {
    char buf[1024];
    char reply[256];
    ssize_t n;
    size_t len = 0;
    while (cs->running && (n = recv(client, buf + len, sizeof(buf) - len - 1, 0)) > 0) {
        len += (size_t)n;
        buf[len] = '\0';
        char *nl;
        while ((nl = strchr(buf, '\n')) != NULL) {
            *nl = '\0';
            handle_line(cs, buf, reply, sizeof(reply));
            send(client, reply, (int)strlen(reply), 0);
            size_t consumed = (size_t)(nl - buf) + 1;
            memmove(buf, buf + consumed, len - consumed);
            len -= consumed;
        }
    }
    closesock(client);
}

#ifdef _WIN32
static DWORD WINAPI listener_thread(LPVOID arg) {
#else
static void *listener_thread(void *arg) {
#endif
    struct control_socket *cs = (struct control_socket *)arg;
    while (cs->running) {
        sock_t client = accept(cs->listener, NULL, NULL);
        if (client == SOCK_INVALID) break;
        serve_client(cs, client);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

control_socket_t *control_socket_start(uint16_t port,
                                       const control_socket_callbacks_t *cb) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#else
    /* A closed client can otherwise kill the host plugin on send(). */
    signal(SIGPIPE, SIG_IGN);
#endif
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) return NULL;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(s, 1) < 0) {
        closesock(s);
        return NULL;
    }

    struct control_socket *cs = calloc(1, sizeof(*cs));
    if (!cs) { closesock(s); return NULL; }
    cs->listener = s;
    cs->cb = *cb;
    cs->running = 1;
#ifdef _WIN32
    cs->thread = CreateThread(NULL, 0, listener_thread, cs, 0, NULL);
#else
    pthread_create(&cs->thread, NULL, listener_thread, cs);
#endif
    return cs;
}

void control_socket_stop(control_socket_t *cs) {
    if (!cs) return;
    cs->running = 0;
    closesock(cs->listener);
#ifdef _WIN32
    WaitForSingleObject(cs->thread, 2000);
    CloseHandle(cs->thread);
    WSACleanup();
#else
    pthread_join(cs->thread, NULL);
#endif
    free(cs);
}
