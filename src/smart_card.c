/* smart_card.c
 *
 * PI Domain 2 smart-card reader emulator. Handles the opcode-based
 * command set I reverse-engineered out of the NPHE kiosk ROM, which
 * earlier writeups in this project incorrectly labelled as "the printer"
 * - jamchamb's 2021 work established that the real Snap Station printer
 * sits on the JoyBus (see joybus_snapstation.c), while this PI DOM2
 * peripheral is kiosk-specific hardware. The credit-card reader is the
 * best match for the UI text ("make sure a print credit exists") and the
 * opcode profile (device-ID query, DMA-read card memory, commit).
 */

#include "smart_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Opcodes (top byte of 32-bit write to COMMAND reg). */
#define OP_PARAM_3C        0x3Cu
#define OP_PARAM_SET_4B    0x4Bu
#define OP_PARAM_78        0x78u
#define OP_READ_A5         0xA5u
#define OP_BLOCK_PREP_B4   0xB4u
#define OP_STATUS_D2       0xD2u
#define OP_DEVICE_ID_E1    0xE1u
#define OP_COMMIT_F0       0xF0u

typedef enum {
    SCS_IDLE,
    SCS_AWAIT_ID_READ,
    SCS_AWAIT_BLOCK_DMA,
    SCS_AWAIT_COMMIT_DMA,
    SCS_AWAIT_READ_DMA,
} sc_state_t;

struct sc_card {
    uint8_t data[SC_CARD_BYTES];
};

struct sc_reader {
    sc_reader_backend_t backend;
    sc_card_t  *card;
    sc_state_t  state;

    uint32_t    device_id;
    uint32_t    last_read_offset;
    uint32_t    last_param_4b;

    uint8_t     write_scratch[SC_BLOCK_BYTES];
    uint8_t     cmd_struct[16];
    size_t      cmd_struct_size;

    uint32_t    status_reg;
    uint32_t    tick;
};

static void sc_log(sc_reader_t *r, const char *fmt, ...)
{
    if (!r || !r->backend.on_event) return;
    char buf[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    r->backend.on_event(buf, r->backend.user);
}

/* ----------------------------------------------------------------------- */
/* Card                                                                     */
/* ----------------------------------------------------------------------- */

sc_card_t *sc_card_create(uint32_t initial_balance)
{
    sc_card_t *c = (sc_card_t*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->data[SC_MAGIC_OFFSET + 0] = 'P';
    c->data[SC_MAGIC_OFFSET + 1] = 'O';
    c->data[SC_MAGIC_OFFSET + 2] = 'K';
    c->data[SC_MAGIC_OFFSET + 3] = 'E';
    c->data[SC_VERSION_OFFSET + 0] = 0x00;
    c->data[SC_VERSION_OFFSET + 1] = 0x01;
    sc_card_set_balance(c, initial_balance);
    return c;
}

void sc_card_destroy(sc_card_t *c) { free(c); }

uint32_t sc_card_balance(const sc_card_t *c)
{
    if (!c) return 0;
    return ((uint32_t)c->data[SC_BALANCE_OFFSET + 0] << 24) |
           ((uint32_t)c->data[SC_BALANCE_OFFSET + 1] << 16) |
           ((uint32_t)c->data[SC_BALANCE_OFFSET + 2] <<  8) |
           ((uint32_t)c->data[SC_BALANCE_OFFSET + 3] <<  0);
}

void sc_card_set_balance(sc_card_t *c, uint32_t b)
{
    if (!c) return;
    c->data[SC_BALANCE_OFFSET + 0] = (uint8_t)(b >> 24);
    c->data[SC_BALANCE_OFFSET + 1] = (uint8_t)(b >> 16);
    c->data[SC_BALANCE_OFFSET + 2] = (uint8_t)(b >>  8);
    c->data[SC_BALANCE_OFFSET + 3] = (uint8_t)(b >>  0);
}

int sc_card_save(const sc_card_t *c, const char *path)
{
    if (!c || !path) return -1;
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    size_t n = fwrite(c->data, 1, SC_CARD_BYTES, f);
    fclose(f);
    return n == SC_CARD_BYTES ? 0 : -1;
}

int sc_card_load(sc_card_t *c, const char *path)
{
    if (!c || !path) return -1;
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    size_t n = fread(c->data, 1, SC_CARD_BYTES, f);
    fclose(f);
    return n == SC_CARD_BYTES ? 0 : -1;
}

/* ----------------------------------------------------------------------- */
/* Reader                                                                   */
/* ----------------------------------------------------------------------- */

sc_reader_t *sc_reader_create(const sc_reader_backend_t *backend)
{
    sc_reader_t *r = (sc_reader_t*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (backend) r->backend = *backend;
    r->device_id  = SC_DEVICE_ID_DEFAULT;
    r->status_reg = SC_STATUS_NO_CARD;      /* no card by default */
    r->state      = SCS_IDLE;
    sc_log(r, "sc: reader created, device_id=0x%08x", r->device_id);
    return r;
}

void sc_reader_destroy(sc_reader_t *r) { free(r); }

void sc_reader_insert(sc_reader_t *r, sc_card_t *c)
{
    if (!r) return;
    r->card = c;
    r->status_reg = SC_STATUS_OK;
    sc_log(r, "sc: card inserted, balance=%u", (unsigned)sc_card_balance(c));
}

void sc_reader_eject(sc_reader_t *r)
{
    if (!r) return;
    r->card = NULL;
    r->status_reg = SC_STATUS_NO_CARD;
    sc_log(r, "sc: card ejected");
}

sc_card_t *sc_reader_current_card(sc_reader_t *r)
{
    return r ? r->card : NULL;
}

/* ----------------------------------------------------------------------- */
/* MMIO                                                                     */
/* ----------------------------------------------------------------------- */

static void handle_cmd(sc_reader_t *r, uint32_t cmd)
{
    uint8_t  op  = (cmd >> 24) & 0xFF;
    uint32_t arg = cmd & 0x00FFFFFFu;

    switch (op) {
    case OP_STATUS_D2:
        sc_log(r, "sc: CMD 0xD2 (status)");
        if (!r->card) r->status_reg = SC_STATUS_NO_CARD;
        else          r->status_reg = SC_STATUS_OK;
        break;

    case OP_DEVICE_ID_E1:
        sc_log(r, "sc: CMD 0xE1 (device id)");
        r->state = SCS_AWAIT_ID_READ;
        r->status_reg = SC_STATUS_OK;
        break;

    case OP_READ_A5:
        sc_log(r, "sc: CMD 0xA5 offset=0x%06x", arg);
        r->last_read_offset = arg;
        r->state = SCS_AWAIT_READ_DMA;
        r->status_reg = SC_STATUS_OK | SC_STATUS_READY_BIT;
        break;

    case OP_BLOCK_PREP_B4:
        sc_log(r, "sc: CMD 0xB4 (prepare write block)");
        r->state = SCS_AWAIT_BLOCK_DMA;
        r->status_reg = SC_STATUS_OK;
        break;

    case OP_COMMIT_F0:
        sc_log(r, "sc: CMD 0xF0 (commit transaction)");
        r->state = SCS_AWAIT_COMMIT_DMA;
        r->status_reg = SC_STATUS_OK;
        break;

    case OP_PARAM_3C:  sc_log(r, "sc: CMD 0x3C");                       break;
    case OP_PARAM_78:  sc_log(r, "sc: CMD 0x78 (commit param)");        break;
    case OP_PARAM_SET_4B:
        sc_log(r, "sc: CMD 0x4B arg=0x%06x", arg);
        r->last_param_4b = arg;
        break;

    default:
        sc_log(r, "sc: CMD UNKNOWN 0x%02x arg=0x%06x", op, arg);
        break;
    }
}

void sc_reader_reg_write32(sc_reader_t *r, uint32_t offset, uint32_t v)
{
    if (!r) return;
    if (offset == SC_STATUS_OFFSET) {
        if (v == 0) {
            sc_log(r, "sc: STATUS=0 (reset)");
            r->state = SCS_IDLE;
            r->status_reg = r->card ? SC_STATUS_OK : SC_STATUS_NO_CARD;
        }
        return;
    }
    if (offset == SC_COMMAND_OFFSET) {
        handle_cmd(r, v);
        return;
    }
}

uint32_t sc_reader_reg_read32(sc_reader_t *r, uint32_t offset)
{
    if (!r) return 0;
    if (offset == SC_STATUS_OFFSET) {
        r->tick++;
        return r->status_reg;
    }
    return 0;
}

/* ----------------------------------------------------------------------- */
/* DMA                                                                      */
/* ----------------------------------------------------------------------- */

void sc_reader_dma_read(sc_reader_t *r, uint32_t offset,
                        uint8_t *dst, size_t size)
{
    (void)offset;
    if (!r || !dst || !size) return;
    memset(dst, 0, size);

    if (r->state == SCS_AWAIT_ID_READ) {
        sc_log(r, "sc: DMA_READ %zu bytes (device id)", size);
        if (size >= 8) {
            dst[0] = 0x00; dst[1] = 0x00; dst[2] = 0x00; dst[3] = 0x00;
            dst[4] = (r->device_id >> 24) & 0xFF;
            dst[5] = (r->device_id >> 16) & 0xFF;
            dst[6] = (r->device_id >>  8) & 0xFF;
            dst[7] = (r->device_id >>  0) & 0xFF;
        }
        r->state = SCS_IDLE;
        return;
    }

    if (r->state == SCS_AWAIT_READ_DMA) {
        sc_log(r, "sc: DMA_READ %zu bytes from card offset 0x%x",
               size, r->last_read_offset);
        if (r->card) {
            for (size_t i = 0; i < size; i++) {
                uint32_t off = (r->last_read_offset + (uint32_t)i) % SC_CARD_BYTES;
                dst[i] = r->card->data[off];
            }
        }
        r->state = SCS_IDLE;
        return;
    }

    sc_log(r, "sc: DMA_READ %zu in unexpected state %d", size, r->state);
}

void sc_reader_dma_write(sc_reader_t *r, uint32_t offset,
                         const uint8_t *src, size_t size)
{
    (void)offset;
    if (!r || !src || !size) return;

    if (r->state == SCS_AWAIT_BLOCK_DMA) {
        size_t n = size < SC_BLOCK_BYTES ? size : SC_BLOCK_BYTES;
        memcpy(r->write_scratch, src, n);
        sc_log(r, "sc: DMA_WRITE %zu bytes to write scratch", n);
        r->state = SCS_IDLE;
        r->status_reg = SC_STATUS_OK;
        return;
    }

    if (r->state == SCS_AWAIT_COMMIT_DMA) {
        size_t n = size < sizeof(r->cmd_struct) ? size : sizeof(r->cmd_struct);
        memcpy(r->cmd_struct, src, n);
        r->cmd_struct_size = n;
        sc_log(r, "sc: DMA_WRITE %zu bytes command struct (commit)", n);

        /* Interpret the commit: if a card is present, decrement credit. */
        if (r->card) {
            uint32_t bal = sc_card_balance(r->card);
            if (bal > 0) {
                sc_card_set_balance(r->card, bal - 1);
                sc_log(r, "sc: credit decremented %u -> %u",
                       (unsigned)bal, (unsigned)(bal - 1));
            } else {
                sc_log(r, "sc: commit with zero balance - denied");
            }
        } else {
            sc_log(r, "sc: commit with no card inserted");
        }

        r->state = SCS_IDLE;
        r->status_reg = SC_STATUS_OK;
        return;
    }

    sc_log(r, "sc: DMA_WRITE %zu in unexpected state %d", size, r->state);
}
