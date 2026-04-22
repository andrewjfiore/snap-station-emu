/* joybus_snapstation.c
 *
 * Platform-agnostic implementation of jamchamb's Snap Station protocol.
 */

#include "joybus_snapstation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

struct jbs_printer {
    jbs_backend_t backend;

    jbs_state_t state;

    /* Last bytes written through each channel. jamchamb's analysis shows
     * the station "stores" what was written and echoes it on read. */
    uint8_t     id_buf[JBS_PAK_BLOCK_SIZE];     /* 0x8000 channel */
    uint8_t     flow_buf[JBS_PAK_BLOCK_SIZE];   /* 0xC000 channel */

    int         photo_idx;           /* 0..16 */
    int         busy_ticks_remaining;
    int         capture_stall_default;

    uint8_t     rolling_response;    /* silly monotone response byte */
};

/* Standard N64 controller-pak CRC-8 (polynomial 0x85). libdragon's
 * joybus_accessory_read/write verify that the response byte in a pak
 * write reply matches this CRC over the 32 data bytes just sent;
 * returning anything else causes libdragon to retry forever or give
 * up with JOYBUS_ACCESSORY_IO_STATUS_BAD_CRC.
 *
 * The algorithm clocks through 33 bytes (32 data + 1 padding zero) MSB
 * first, XORing 0x85 into the running CRC whenever bit 7 is set before
 * the shift. */
uint8_t jbs_pak_crc(const uint8_t data[32])
{
    uint8_t crc = 0;
    for (int i = 0; i < 33; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            uint8_t xor_tap = (crc & 0x80) ? 0x85 : 0;
            crc <<= 1;
            if (i < 32 && ((data[i] >> bit) & 1)) crc |= 1;
            crc ^= xor_tap;
        }
    }
    return crc;
}

static void jbs_log(jbs_printer_t *p, const char *fmt, ...)
{
    if (!p || !p->backend.on_event) return;
    char buf[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    p->backend.on_event(buf, p->backend.user);
}

/* ----------------------------------------------------------------------- */

jbs_printer_t *jbs_create(const jbs_backend_t *backend)
{
    jbs_printer_t *p = (jbs_printer_t*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    if (backend) p->backend = *backend;
    p->state = JBS_IDLE;
    p->capture_stall_default = 30;     /* ~0.5 s at 60 Hz */
    p->rolling_response = 0x27;
    jbs_log(p, "jbs: station created");
    return p;
}

void jbs_destroy(jbs_printer_t *p)
{
    free(p);
}

jbs_state_t jbs_state(const jbs_printer_t *p) { return p ? p->state : JBS_IDLE; }
int         jbs_photo_index(const jbs_printer_t *p) { return p ? p->photo_idx : 0; }

void jbs_set_capture_stall(jbs_printer_t *p, int t)
{
    if (p) p->capture_stall_default = (t < 0) ? 0 : t;
}

void jbs_tick(jbs_printer_t *p)
{
    if (!p) return;
    if (p->busy_ticks_remaining > 0) p->busy_ticks_remaining--;
}

void jbs_console_reset(jbs_printer_t *p)
{
    if (!p) return;
    jbs_log(p, "jbs: console reset signalled");
    /* Stay armed (the station is still plugged in across a reset), but
     * move past AWAIT_RESET so the boot-time handshake proceeds. */
    if (p->state == JBS_AWAIT_RESET) p->state = JBS_IDLE;
    p->photo_idx = 0;
    p->busy_ticks_remaining = 0;
}

/* ----------------------------------------------------------------------- */
/* JoyBus entry points                                                     */
/* ----------------------------------------------------------------------- */

int jbs_joybus_ident(jbs_printer_t *p, uint8_t out[3])
{
    if (!p || !out) return -1;
    /* Standard N64 controller ident: 0x0500 (controller) + status.
     * Setting the pak-present bit tells the game a pak is inserted. */
    out[0] = JBS_IDENT_HI;
    out[1] = JBS_IDENT_LO;
    out[2] = JBS_STATUS_PAK_PRESENT;
    return 0;
}

static uint8_t trailing_byte(const uint8_t buf[32])
{
    /* jamchamb observed the last byte of the 32-byte write is the
     * meaningful signal; everything else is zero padding. */
    return buf[31];
}

uint8_t jbs_joybus_pak_read(jbs_printer_t *p, uint16_t addr, uint8_t out[32])
{
    if (!p || !out) return 0;
    uint16_t ch = addr & 0xF800u;       /* Pak address is aligned to 32 */

    if (ch == JBS_ADDR_ID) {
        memcpy(out, p->id_buf, JBS_PAK_BLOCK_SIZE);
        jbs_log(p, "jbs: READ  0x8000 -> trail=0x%02x", out[31]);
        return 0x00;
    }
    if (ch == JBS_ADDR_FLOW) {
        /* If we're busy-stalling, override the last byte with 0x08 so
         * the ROM spins in its busy loop. The post-0x02 busy stall is
         * gated on capture_stall_default so a caller that set it to 0
         * (e.g. the HLE input plugin) can fully disable the override
         * without relying on jbs_tick to drain busy_ticks_remaining in
         * time. AWAIT_RESET always returns busy to keep the handshake
         * protocol correct. */
        memcpy(out, p->flow_buf, JBS_PAK_BLOCK_SIZE);
        int stall_active = (p->capture_stall_default > 0
                            && p->busy_ticks_remaining > 0);
        if (stall_active || p->state == JBS_AWAIT_RESET) {
            out[31] = JBS_FLOW_BUSY;
        }
        jbs_log(p, "jbs: READ  0xC000 -> trail=0x%02x (state=%d bt=%d)",
                out[31], p->state, p->busy_ticks_remaining);
        return 0x00;
    }

    if (ch == JBS_ADDR_CREDIT) {
        /* HLE: emulator reports the smart-card balance so the ROM can
         * gate print on credits. Big-endian u32 in the last 4 bytes. */
        memset(out, 0, JBS_PAK_BLOCK_SIZE);
        uint32_t bal = 0;
        if (p->backend.query_credit_balance) {
            bal = p->backend.query_credit_balance(p->backend.user);
        }
        out[28] = (uint8_t)(bal >> 24);
        out[29] = (uint8_t)(bal >> 16);
        out[30] = (uint8_t)(bal >>  8);
        out[31] = (uint8_t)(bal);
        jbs_log(p, "jbs: READ  0xD000 -> balance=%u", (unsigned)bal);
        return 0x00;
    }

    memset(out, 0, JBS_PAK_BLOCK_SIZE);
    return 0x00;
}

uint8_t jbs_joybus_pak_write(jbs_printer_t *p, uint16_t addr, const uint8_t in[32])
{
    if (!p || !in) return 0;
    uint16_t ch = addr & 0xF800u;
    uint8_t  t  = trailing_byte(in);
    uint8_t  r  = p->rolling_response++;

    if (ch == JBS_ADDR_ID) {
        memcpy(p->id_buf, in, JBS_PAK_BLOCK_SIZE);
        if (t == 0xFEu) {
            jbs_log(p, "jbs: WRITE 0x8000 reset probe (FE)");
            p->state = JBS_ARMED;
        } else if (t == JBS_PAK_ID) {
            jbs_log(p, "jbs: WRITE 0x8000 pak-ID probe (85) -- station confirmed");
            /* Nothing to do beyond storing in id_buf - the ROM reads it
             * back and sees 0x85 to confirm us as a Snap Station. */
        } else {
            jbs_log(p, "jbs: WRITE 0x8000 trail=0x%02x", t);
        }
        return r;
    }

    if (ch == JBS_ADDR_FLOW) {
        memcpy(p->flow_buf, in, JBS_PAK_BLOCK_SIZE);
        jbs_log(p, "jbs: WRITE 0xC000 trail=0x%02x (state=%d)", t, p->state);

        switch (t) {
        case JBS_FLOW_PRE_SAVE:        /* 0xCC */
            p->state = JBS_SAVING;
            break;

        case JBS_FLOW_POST_SAVE:       /* 0x33 */
            p->state = JBS_SAVE_DONE;
            break;

        case JBS_FLOW_REQ_RESET:       /* 0x5A */
            p->state = JBS_AWAIT_RESET;
            if (p->backend.on_request_reset)
                p->backend.on_request_reset(p->backend.user);
            break;

        case JBS_FLOW_DISPLAY_BEGIN:   /* 0x01 */
            /* A new photo session is only legal from a "clean" state:
             * fresh boot (IDLE), after the FE/85 handshake (ARMED), after
             * a completed Gallery-Print save sequence (SAVE_DONE), or
             * immediately after a previous session wrapped up
             * (PHOTO_MODE_END). Reject from any other state so a stray
             * 0x01 during a capture doesn't reset photo_idx mid-job. */
            if (p->state != JBS_IDLE && p->state != JBS_ARMED &&
                p->state != JBS_SAVE_DONE && p->state != JBS_PHOTO_MODE_END) {
                jbs_log(p, "jbs: DISPLAY_BEGIN rejected (state=%d)",
                        p->state);
                break;
            }
            p->state = JBS_PHOTO_MODE_START;
            p->photo_idx = 0;
            break;

        case JBS_FLOW_DISPLAY_PHOTO: { /* 0x02 */
            if (p->state != JBS_PHOTO_MODE_START &&
                p->state != JBS_PHOTO_CAPTURE) {
                jbs_log(p, "jbs: DISPLAY_PHOTO rejected (state=%d)",
                        p->state);
                break;
            }
            int idx = p->photo_idx;
            if (idx < 16) {
                if (p->backend.on_capture_photo)
                    p->backend.on_capture_photo(idx, p->backend.user);
                p->photo_idx = idx + 1;
                p->state = JBS_PHOTO_CAPTURE;
                /* Install the "busy while the printer captures" stall. */
                p->busy_ticks_remaining = p->capture_stall_default;
            } else {
                jbs_log(p, "jbs: DISPLAY_PHOTO overflow (idx=%d)", idx);
            }
        } break;

        case JBS_FLOW_DISPLAY_END:     /* 0x04 */
            /* Only fire the printer if we actually captured a full sheet
             * of 16 photos. A stray 0x04, a duplicate, or a 0x04 arriving
             * before all 0x02s have been handled must not trigger an
             * unwanted print dialog or credit decrement. */
            if (p->state != JBS_PHOTO_CAPTURE || p->photo_idx != 16) {
                jbs_log(p,
                    "jbs: DISPLAY_END rejected (state=%d photo_idx=%d/16)",
                    p->state, p->photo_idx);
                break;
            }
            p->state = JBS_PHOTO_MODE_END;
            if (p->backend.on_print_ready)
                p->backend.on_print_ready(p->backend.user);
            break;

        default:
            /* Unknown code. Some pre-/post-print synchronisation bytes
             * may use other values we haven't documented; store and
             * ignore. */
            break;
        }
        return r;
    }

    return r;
}
