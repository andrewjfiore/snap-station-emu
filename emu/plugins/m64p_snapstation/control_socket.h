/* control_socket.h
 *
 * Cross-platform TCP control surface for the Mupen64Plus Snap Station
 * input plugin. Loopback-only. See README.md for the wire format.
 */
#ifndef SNAP_STATION_CONTROL_SOCKET_H
#define SNAP_STATION_CONTROL_SOCKET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct control_socket control_socket_t;

/* Callback table the plugin registers so the socket can drive the
 * existing joybus / smart_card modules without this file importing
 * them directly (keeps test builds free of m64p headers). */
typedef struct {
    void (*insert_card)(uint32_t credits);
    void (*remove_card)(void);
    uint32_t (*get_balance)(void);
    void (*press_print)(void);
    void (*photo_ready)(void);
    void (*end_print)(void);
    uint8_t (*peek_flow)(void);
} control_socket_callbacks_t;

/* Start the server on 127.0.0.1:port. Returns NULL on error. The server
 * spawns one background thread that accepts a single client at a time. */
control_socket_t *control_socket_start(uint16_t port,
                                       const control_socket_callbacks_t *cb);

/* Block on pending requests and return after the next one is handled.
 * Most callers instead let the background thread drive things and poll
 * shared state from the main loop. Returns false if the socket has been
 * shut down. */
bool control_socket_pump(control_socket_t *cs);

/* Gracefully shut down. Safe to call from any thread. */
void control_socket_stop(control_socket_t *cs);

#ifdef __cplusplus
}
#endif

#endif
