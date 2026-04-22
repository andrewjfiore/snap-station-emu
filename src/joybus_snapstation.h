/* joybus_snapstation.h
 *
 * JoyBus-level emulation of the Snap Station for N64 controller port 4.
 * Implements the protocol jamchamb reverse-engineered in 2021
 * (https://jamchamb.net/2021/08/17/snap-station.html):
 *
 *   - Presents as a controller (ID 0x0500) with a pak inserted (0x85).
 *   - 0x8000 channel: FE reset + 85 pak-ID handshake.
 *   - 0xC000 channel: print-flow state machine.
 *       CC (pre-save) -> 33 (post-save) -> 5A (request reset) ->
 *       [console resets] -> 01 (photo mode start) -> 02 x16 (each photo)
 *       -> 04 (done).
 *   - Returns 0x08 busy on reads when we want to stall the ROM.
 *
 * Photo capture happens OUTSIDE this module. When the state machine
 * observes a 0x02 event, it fires the on_capture_photo backend callback;
 * that callback is responsible for grabbing the current N64 framebuffer
 * and buffering it. When 0x04 arrives, on_print_ready fires and the
 * backend hands the 16 accumulated frames to the sticker compositor.
 *
 * This file is platform-agnostic. The Mupen64Plus-specific input-plugin
 * scaffold is in m64p_input_plugin.c.
 */

#ifndef JOYBUS_SNAPSTATION_H
#define JOYBUS_SNAPSTATION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol constants. */
#define JBS_ADDR_ID           0x8000u
#define JBS_ADDR_FLOW         0xC000u
#define JBS_PAK_ID            0x85u
#define JBS_RUMBLE_ID         0x80u

#define JBS_FLOW_PRE_SAVE     0xCCu
#define JBS_FLOW_POST_SAVE    0x33u
#define JBS_FLOW_REQ_RESET    0x5Au
#define JBS_FLOW_DISPLAY_BEGIN 0x01u
#define JBS_FLOW_DISPLAY_PHOTO 0x02u
#define JBS_FLOW_DISPLAY_END   0x04u
#define JBS_FLOW_BUSY          0x08u

/* HLE extension channel: the input plugin reports the current smart-
 * card credit balance when the ROM reads from this channel. Not part
 * of real Snap Station hardware - it's an emulator-side convenience so
 * a custom ROM can gate printing on credits without needing a PI DOM2
 * core hook. Reads return a 32-byte block with the balance as a big-
 * endian uint32 in bytes[28..31]. */
#define JBS_ADDR_CREDIT        0xD000u

/* Standard N64 controller-status word: ID 0x0500 = standard controller;
 * status 0x01 = accessory present. */
#define JBS_IDENT_HI          0x05u
#define JBS_IDENT_LO          0x00u
#define JBS_STATUS_PAK_PRESENT 0x01u

#define JBS_PAK_BLOCK_SIZE    32        /* pak read/write data size */

/* State of the print flow. */
typedef enum {
    JBS_IDLE = 0,
    JBS_ARMED,              /* got FE probe, waiting for 85 */
    JBS_SAVING,             /* got CC */
    JBS_SAVE_DONE,          /* got 33 */
    JBS_AWAIT_RESET,        /* got 5A, busy-stalling until reset */
    JBS_PHOTO_MODE_START,   /* got 01 after reset */
    JBS_PHOTO_CAPTURE,      /* in loop of 02, index counted in photo_idx */
    JBS_PHOTO_MODE_END,     /* got 04 */
} jbs_state_t;

/* Backend hooks. All are optional (NULL-checked before call). */
typedef struct {
    /* Called each time the ROM sends 0x02 (photo ready). idx is 0..15.
     * The backend should capture the current framebuffer. */
    void (*on_capture_photo)(int idx, void *user);

    /* Called when the ROM sends 0x04 (photo display end). The backend
     * should composite the 16 captured frames and fire the print
     * dialog. */
    void (*on_print_ready)(void *user);

    /* Called when the ROM sends 0x5A (request reset). The backend
     * should trigger an emulator soft reset, or let the stall continue. */
    void (*on_request_reset)(void *user);

    /* Called on any protocol event for logging. */
    void (*on_event)(const char *msg, void *user);

    /* HLE extension: return the current smart-card balance (in number
     * of credits) when the ROM reads from JBS_ADDR_CREDIT. Returns 0
     * if no card is inserted. Optional - NULL means "always zero". */
    uint32_t (*query_credit_balance)(void *user);

    void *user;
} jbs_backend_t;

/* The Snap Station virtual device. */
typedef struct jbs_printer jbs_printer_t;

/* Lifecycle. */
jbs_printer_t *jbs_create(const jbs_backend_t *backend);
void           jbs_destroy(jbs_printer_t *p);

/* Inspect state (mainly for tests). */
jbs_state_t    jbs_state(const jbs_printer_t *p);
int            jbs_photo_index(const jbs_printer_t *p);

/* JoyBus command entry points. These mirror Mupen64Plus's input-plugin
 * ControllerCommand/ReadController contract, but are platform-agnostic. */

/* Controller identification (JoyBus cmd 0x00 / 0xFF). Fills out[3] with
 * ID_HI, ID_LO, STATUS. Returns 0 on success. */
int jbs_joybus_ident(jbs_printer_t *p, uint8_t out[3]);

/* Pak read (JoyBus cmd 0x02). addr is 16-bit with CRC-5 in low bits;
 * we mask to align(0x20) internally. Fills out[32] with data bytes and
 * returns a CRC-8 stub (0x00 - Mupen64Plus computes its own CRC). */
uint8_t jbs_joybus_pak_read (jbs_printer_t *p, uint16_t addr, uint8_t out[32]);

/* Pak write (JoyBus cmd 0x03). Consumes in[32] at addr. Returns the
 * response status byte to put in the reply CRC slot (jamchamb's captures
 * show 0x27, 0xAA, etc.; we return a simple rolling value). */
uint8_t jbs_joybus_pak_write(jbs_printer_t *p, uint16_t addr, const uint8_t in[32]);

/* Simulate the console being reset. Call from the backend on_request_reset
 * handler (or on real emulator reset) to move the state machine past
 * AWAIT_RESET. */
void jbs_console_reset(jbs_printer_t *p);

/* Standard N64 controller-pak CRC-8 (polynomial 0x85) over a 32-byte
 * block. Used by the Mupen64Plus input-plugin scaffold to fill the
 * response byte of pak-read/pak-write JoyBus replies so libdragon's
 * CRC check passes. */
uint8_t jbs_pak_crc(const uint8_t data[32]);

/* Tick the busy-stall timer. Call every frame. Governs how long the
 * station returns 0x08 busy after a 0x02 photo signal to give the
 * "printer" time to capture. */
void jbs_tick(jbs_printer_t *p);

/* Override how many ticks each 0x02 stalls for (default ~30 = half
 * second at 60 Hz). Set 0 for no stall. */
void jbs_set_capture_stall(jbs_printer_t *p, int ticks);

#ifdef __cplusplus
}
#endif

#endif /* JOYBUS_SNAPSTATION_H */
