/* test_joybus.c
 *
 * Exercise the JoyBus Snap Station state machine with the exact sequence
 * jamchamb documented. Validates the full print flow:
 *
 *   1. FE reset + 85 pak-ID handshake
 *   2. Gallery press: CC -> 33 -> 5A
 *   3. Console reset
 *   4. Photo display: 01, 02 x16, 04
 *
 * Captures fire on_capture_photo and end-of-display fires on_print_ready.
 */

#include "../src/joybus_snapstation.h"

#include <stdio.h>
#include <string.h>

static int captures = 0;
static int print_fired = 0;

static void on_event(const char *msg, void *user) {
    (void)user;
    printf("  | %s\n", msg);
}
static void on_capture(int idx, void *user) {
    (void)user;
    printf("    *** BACKEND capture photo %d ***\n", idx);
    captures++;
}
static void on_print(void *user) {
    (void)user;
    printf("    *** BACKEND print ready - compose sheet ***\n");
    print_fired++;
}
static void on_reset(void *user) {
    (void)user;
    printf("    *** BACKEND reset request - emulator should soft-reset ***\n");
}

/* Build a 32-byte pak write frame whose only meaningful byte is the
 * trailing one. */
static void frame(uint8_t trail, uint8_t out[32]) {
    memset(out, 0, 32);
    out[31] = trail;
}

int main(void)
{
    jbs_backend_t b;
    memset(&b, 0, sizeof(b));
    b.on_event         = on_event;
    b.on_capture_photo = on_capture;
    b.on_print_ready   = on_print;
    b.on_request_reset = on_reset;

    jbs_printer_t *p = jbs_create(&b);
    jbs_set_capture_stall(p, 0);   /* no busy stall in this test */

    /* --- Controller ident --- */
    uint8_t ident[3];
    jbs_joybus_ident(p, ident);
    printf("\n[ident] %02x %02x %02x  (expect 05 00 01)\n",
           ident[0], ident[1], ident[2]);

    /* --- Handshake at 0x8000 --- */
    uint8_t buf[32];
    printf("\n=== Handshake ===\n");
    frame(0xFE, buf);
    jbs_joybus_pak_write(p, 0x8001, buf);          /* FE reset */
    jbs_joybus_pak_read (p, 0x8001, buf);          /* should return last-written */
    frame(0x85, buf);
    jbs_joybus_pak_write(p, 0x8001, buf);          /* 85 pak-ID */
    jbs_joybus_pak_read (p, 0x8001, buf);
    printf("  post-handshake trail = 0x%02x (expect 85)\n", buf[31]);

    /* --- Gallery press: CC -> 33 -> 5A --- */
    printf("\n=== Gallery Print press ===\n");
    frame(0xCC, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    frame(0x33, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    frame(0x5A, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    printf("  state = %d (expect JBS_AWAIT_RESET=4)\n", jbs_state(p));

    /* Read while in AWAIT_RESET should return busy 0x08. */
    jbs_joybus_pak_read(p, 0xC01B, buf);
    printf("  busy read trail = 0x%02x (expect 08)\n", buf[31]);

    /* --- Console resets --- */
    printf("\n=== Console reset ===\n");
    jbs_console_reset(p);
    printf("  state after reset = %d (expect JBS_IDLE=0)\n", jbs_state(p));

    /* --- Photo display: 01, 02 x16, 04 --- */
    printf("\n=== Photo display mode ===\n");
    frame(0x01, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    for (int i = 0; i < 16; i++) {
        frame(0x02, buf);
        jbs_joybus_pak_write(p, 0xC01B, buf);
    }
    frame(0x04, buf); jbs_joybus_pak_write(p, 0xC01B, buf);

    printf("\n=== Final state ===\n");
    printf("  captures fired:   %d (expect 16)\n", captures);
    printf("  print_ready:      %d (expect 1)\n",  print_fired);
    printf("  photo_index:      %d (expect 16)\n", jbs_photo_index(p));
    printf("  state:            %d (expect JBS_PHOTO_MODE_END=7)\n", jbs_state(p));

    int ok = (captures == 16) && (print_fired == 1) &&
             (jbs_photo_index(p) == 16) &&
             (jbs_state(p) == JBS_PHOTO_MODE_END);

    /* --- State-machine guard tests ------------------------------------
     * After the happy path, the station must reject out-of-order flow
     * commands so that stray bytes or buggy ROMs can't trigger capture
     * or print. These go through the SAME station instance so we also
     * exercise "recovery" (DISPLAY_BEGIN is legal again from
     * PHOTO_MODE_END to start a fresh session). */
    printf("\n=== Guard tests ===\n");
    int guard_ok = 1;
    int baseline_captures = captures;
    int baseline_prints   = print_fired;

    /* 1. Start a fresh session (DISPLAY_BEGIN from PHOTO_MODE_END is OK). */
    frame(0x01, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    if (jbs_state(p) != JBS_PHOTO_MODE_START) {
        printf("  FAIL: BEGIN from PHOTO_MODE_END rejected\n"); guard_ok = 0;
    }

    /* 2. Partial fill (only 3 photos), then a premature 0x04 must NOT
     * trigger on_print_ready. */
    for (int i = 0; i < 3; i++) {
        frame(0x02, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    }
    frame(0x04, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    if (print_fired != baseline_prints) {
        printf("  FAIL: premature DISPLAY_END (3/16) fired print\n");
        guard_ok = 0;
    }
    if (captures != baseline_captures + 3) {
        printf("  FAIL: 3 captures expected, got %d\n",
               captures - baseline_captures);
        guard_ok = 0;
    }

    /* 3. DISPLAY_PHOTO from an unrelated state (SAVING) must be ignored. */
    jbs_console_reset(p);                     /* back to IDLE */
    frame(0xCC, buf); jbs_joybus_pak_write(p, 0xC01B, buf);   /* SAVING */
    int cap_before = captures;
    frame(0x02, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    if (captures != cap_before) {
        printf("  FAIL: DISPLAY_PHOTO fired outside photo mode\n");
        guard_ok = 0;
    }

    /* 4. Stray DISPLAY_END from SAVING must not fire print. */
    int pr_before = print_fired;
    frame(0x04, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    if (print_fired != pr_before) {
        printf("  FAIL: stray DISPLAY_END from SAVING fired print\n");
        guard_ok = 0;
    }

    /* 5. Duplicate DISPLAY_END after a full sheet must not fire twice.
     * We're currently in SAVING from test 3; jbs_console_reset only
     * breaks AWAIT_RESET, so finish the save cycle first. 0x33 moves
     * us to SAVE_DONE, from which DISPLAY_BEGIN is legal again. */
    frame(0x33, buf); jbs_joybus_pak_write(p, 0xC01B, buf);   /* SAVE_DONE */
    frame(0x01, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    if (jbs_state(p) != JBS_PHOTO_MODE_START) {
        printf("  FAIL: could not re-enter photo mode for dup-end test\n");
        guard_ok = 0;
    }
    int caps_before_dup = captures;
    for (int i = 0; i < 16; i++) {
        frame(0x02, buf); jbs_joybus_pak_write(p, 0xC01B, buf);
    }
    if (captures != caps_before_dup + 16) {
        printf("  FAIL: expected 16 captures in dup-end session, got %d\n",
               captures - caps_before_dup);
        guard_ok = 0;
    }
    int prints_before = print_fired;
    frame(0x04, buf); jbs_joybus_pak_write(p, 0xC01B, buf);   /* legal 04 */
    if (print_fired != prints_before + 1) {
        printf("  FAIL: legal DISPLAY_END did not fire print\n");
        guard_ok = 0;
    }
    int prints_after_end = print_fired;
    frame(0x04, buf); jbs_joybus_pak_write(p, 0xC01B, buf);   /* dup 04 */
    if (print_fired != prints_after_end) {
        printf("  FAIL: duplicate DISPLAY_END fired print a second time\n");
        guard_ok = 0;
    }

    printf("  guards: %s\n", guard_ok ? "PASS" : "FAIL");
    ok = ok && guard_ok;

    jbs_destroy(p);
    printf("\nRESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
