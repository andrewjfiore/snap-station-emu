/* test_smart_card.c
 *
 * Exercise the PI DOM2 smart-card reader with the exact sequence the
 * NPHE ROM's init + credit-check code produces.
 */

#include "../src/smart_card.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void on_event(const char *msg, void *user) {
    (void)user;
    printf("  | %s\n", msg);
}

static void cmd(sc_reader_t *r, uint8_t op, uint32_t arg) {
    uint32_t w = ((uint32_t)op << 24) | (arg & 0xFFFFFFu);
    printf("> CMD 0x%02x (arg=0x%06x)\n", op, arg);
    sc_reader_reg_write32(r, SC_COMMAND_OFFSET, w);
}

static void status_read(sc_reader_t *r, const char *label) {
    uint32_t s = sc_reader_reg_read32(r, SC_STATUS_OFFSET);
    printf("> status = 0x%08x  (%s)\n", s, label);
}

int main(void)
{
    sc_reader_backend_t b;
    memset(&b, 0, sizeof(b));
    b.on_event = on_event;

    sc_reader_t *r = sc_reader_create(&b);
    sc_card_t   *card = sc_card_create(3);   /* 3 credits */

    /* Phase 1: probe with no card inserted. */
    printf("\n=== Phase 1: no card ===\n");
    cmd(r, 0xD2, 0);
    status_read(r, "expect 0x40 NO_CARD");

    /* Phase 2: insert a card and query device id. */
    printf("\n=== Phase 2: card inserted ===\n");
    sc_reader_insert(r, card);
    cmd(r, 0xD2, 0);
    status_read(r, "expect 0x04 OK");

    cmd(r, 0xE1, 0);
    uint8_t id_buf[8] = { 0 };
    sc_reader_dma_read(r, 0, id_buf, 8);
    printf("> device id bytes: ");
    for (int i = 0; i < 8; i++) printf("%02x ", id_buf[i]);
    printf(" (expect 00 00 00 00 00 c2 00 1e)\n");

    /* Phase 3: read card memory at the balance offset. */
    printf("\n=== Phase 3: read card balance ===\n");
    cmd(r, 0xA5, SC_BALANCE_OFFSET);
    uint8_t bal_buf[4] = { 0 };
    sc_reader_dma_read(r, 0, bal_buf, 4);
    uint32_t read_bal = ((uint32_t)bal_buf[0] << 24) |
                        ((uint32_t)bal_buf[1] << 16) |
                        ((uint32_t)bal_buf[2] <<  8) |
                        ((uint32_t)bal_buf[3] <<  0);
    printf("> balance bytes: %02x %02x %02x %02x = %u  (expect 3)\n",
           bal_buf[0], bal_buf[1], bal_buf[2], bal_buf[3], read_bal);

    /* Phase 4: commit a transaction - should decrement credit. */
    printf("\n=== Phase 4: commit (decrement credit) ===\n");
    uint32_t before = sc_card_balance(card);
    cmd(r, 0xF0, 0);
    uint8_t cmd_struct[16] = { 0xde, 0xad, 0xbe, 0xef };
    sc_reader_dma_write(r, 0, cmd_struct, sizeof(cmd_struct));
    uint32_t after = sc_card_balance(card);
    printf("> balance %u -> %u  (expect %u -> %u)\n",
           before, after, before, before - 1);

    /* Phase 5: two more commits to exhaust credit. */
    printf("\n=== Phase 5: exhaust credit ===\n");
    for (int i = 0; i < 5; i++) {
        cmd(r, 0xF0, 0);
        sc_reader_dma_write(r, 0, cmd_struct, sizeof(cmd_struct));
    }
    printf("> final balance: %u  (expect 0)\n",
           (unsigned)sc_card_balance(card));

    /* Phase 6: save + reload. */
    printf("\n=== Phase 6: persistence ===\n");
    sc_card_save(card, "/tmp/test_card.bin");
    sc_card_t *reloaded = sc_card_create(99);
    sc_card_load(reloaded, "/tmp/test_card.bin");
    printf("> reloaded balance: %u  (expect 0)\n",
           (unsigned)sc_card_balance(reloaded));

    int ok = (read_bal == 3) &&
             (id_buf[5] == 0xC2 && id_buf[6] == 0x00 && id_buf[7] == 0x1E) &&
             (after == before - 1) &&
             (sc_card_balance(card) == 0) &&
             (sc_card_balance(reloaded) == 0);

    sc_card_destroy(reloaded);
    sc_card_destroy(card);
    sc_reader_destroy(r);

    printf("\nRESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
