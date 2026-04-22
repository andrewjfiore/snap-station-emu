/* smart_card.h
 *
 * Emulator for the Pokemon credit smart card reader on the Snap Station
 * kiosk's cart bus (PI Domain 2, physical 0x08000000).
 *
 * This is distinct from the JoyBus printer (joybus_snapstation.h). The
 * NPHE Snap Station kiosk ROM accesses this device directly via the
 * libultra osEPiWriteIo / osEPiReadIo / osEPiStartDma primitives. It is
 * the hardware that decrements a player's credit balance before allowing
 * a print job to be triggered.
 *
 * Protocol (reverse-engineered from the NPHE ROM - see docs/PROTOCOL.md):
 *
 *   base+0x00000   STATUS (R/W)    read status byte, write 0 to reset
 *   base+0x10000   COMMAND (W)     32-bit write, top byte = opcode:
 *
 *     0xD2            status query
 *     0xE1            device-id request   -> 8-byte DMA read
 *     0xA5 [offset]   read card memory    -> N-block DMA read
 *     0xB4            prepare write block -> 128-byte DMA write
 *     0xF0            commit transaction  -> 16-byte DMA write of command struct
 *     0x3C            mode param 1
 *     0x4B [arg]      parameter set
 *     0x78            param commit
 *     0x00 (to base)  reset
 *
 * Status bit 0 = busy, bit 2 = ready; byte 0x04 = OK.
 */

#ifndef SMART_CARD_H
#define SMART_CARD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Addressing. */
#define SC_PHYS_BASE          0x08000000u
#define SC_PHYS_SIZE          0x00020000u
#define SC_STATUS_OFFSET      0x00000u
#define SC_COMMAND_OFFSET     0x10000u

/* Card storage size (one Pokemon credit card). */
#define SC_CARD_BYTES         512
#define SC_BLOCK_BYTES        128

/* Device ID returned by 0xE1. 0x00C2001E is the 64-byte-block variant
 * the NPHE init code prefers. */
#define SC_DEVICE_ID_DEFAULT  0x00C2001Eu

/* Status byte values. */
#define SC_STATUS_OK          0x04u
#define SC_STATUS_BUSY_BIT    0x01u
#define SC_STATUS_READY_BIT   0x04u
#define SC_STATUS_NO_CARD     0x40u

/* Card balance is stored big-endian at this offset in the card's
 * 512-byte memory image. */
#define SC_BALANCE_OFFSET     0x10
#define SC_MAGIC_OFFSET       0x00    /* "POKE" ASCII */
#define SC_VERSION_OFFSET     0x04    /* u16 version */

/* Opaque card type. */
typedef struct sc_card sc_card_t;

/* Create a new card with a starting balance (number of prints). */
sc_card_t *sc_card_create(uint32_t initial_balance);
void       sc_card_destroy(sc_card_t *c);

/* Serialise/deserialise a card to a 512-byte blob (for persistence). */
int        sc_card_save(const sc_card_t *c, const char *path);
int        sc_card_load(sc_card_t *c, const char *path);

/* Read current credit balance without modifying. */
uint32_t   sc_card_balance(const sc_card_t *c);

/* Force-set balance (debug/admin). */
void       sc_card_set_balance(sc_card_t *c, uint32_t b);

/* -------------------- reader device -------------------- */

typedef struct sc_reader sc_reader_t;

typedef void (*sc_on_event_fn)(const char *msg, void *user);

typedef struct {
    sc_on_event_fn on_event;
    void          *user;
} sc_reader_backend_t;

sc_reader_t *sc_reader_create(const sc_reader_backend_t *backend);
void         sc_reader_destroy(sc_reader_t *r);

/* Insert / eject a card. Only one card at a time; insert replaces. */
void         sc_reader_insert(sc_reader_t *r, sc_card_t *card);
void         sc_reader_eject (sc_reader_t *r);
sc_card_t   *sc_reader_current_card(sc_reader_t *r);

/* CPU side: 32-bit register access. offset is relative to SC_PHYS_BASE. */
void     sc_reader_reg_write32(sc_reader_t *r, uint32_t offset, uint32_t v);
uint32_t sc_reader_reg_read32 (sc_reader_t *r, uint32_t offset);

/* CPU side: PI DMA. offset relative to SC_PHYS_BASE. */
void     sc_reader_dma_write(sc_reader_t *r, uint32_t offset,
                             const uint8_t *src, size_t size);
void     sc_reader_dma_read (sc_reader_t *r, uint32_t offset,
                             uint8_t *dst, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SMART_CARD_H */
