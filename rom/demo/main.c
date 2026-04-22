/* main.c - Snap Station HLE demo ROM (interactive kiosk loop)
 *
 * Each iteration of the outer loop is one kiosk "session":
 *
 *   1. Splash ("press any button to begin").
 *   2. Probe port 4 for the Snap Station via FE/85 handshake.
 *   3. Show credit balance (the mupen64plus input plugin pre-inserts
 *      9 credits at PluginStartup). If zero, wait for Minus press on
 *      port 1 to top up; otherwise, press A to start.
 *   4. Run the 16-photo flow. The plugin captures a PNG per frame via
 *      CoreDoCommand(M64CMD_TAKE_NEXT_SCREENSHOT), composes a hagaki
 *      sheet after signal_end, opens the Windows print dialog, and
 *      decrements the balance by one.
 *   5. DONE page - press any button to return to the splash.
 *
 * Input layout (via the chained mupen64plus-input-sdl plugin on ports
 * 1-3, matching the HIDAPI Pro Controller ground-truth from test/jstest.c):
 *
 *   A        start print flow (from credit screen) / acknowledge (any screen)
 *   Minus    have the emulator's plugin insert another credit card
 *            (handled entirely in the plugin - not a ROM-visible event)
 */

#include <libdragon.h>
#include <stdio.h>
#include <string.h>

#include "snapstation.h"

/* 16 hue-stepped background colors for visible variety on the sheet. */
static const uint8_t s_bg_rgb[16][3] = {
    {255,  64,  64}, {255, 128,  32}, {255, 192,  32}, {192, 255,  32},
    { 96, 255,  32}, { 32, 255,  96}, { 32, 255, 192}, { 32, 192, 255},
    { 64, 128, 255}, { 96,  64, 255}, {160,  64, 255}, {224,  64, 224},
    {255,  64, 160}, {160, 160, 160}, {224, 224,  96}, { 96, 224, 224},
};

static void draw_photo_frame(int i)
{
    uint32_t bg = graphics_make_color(
        s_bg_rgb[i][0], s_bg_rgb[i][1], s_bg_rgb[i][2], 255);
    uint32_t fg = graphics_make_color(255, 255, 255, 255);

    surface_t *d = display_get();
    graphics_fill_screen(d, bg);
    graphics_set_color(fg, bg);

    char label[16];
    snprintf(label, sizeof(label), "PHOTO %02d", i + 1);
    graphics_draw_text(d, 128, 116, label);

    char tag[8];
    snprintf(tag, sizeof(tag), "%d", i + 1);
    graphics_draw_text(d,   8,   8, tag);
    graphics_draw_text(d, 304,   8, tag);
    graphics_draw_text(d,   8, 224, tag);
    graphics_draw_text(d, 304, 224, tag);

    display_show(d);
}

static void show_console_page(const char *title, const char *body)
{
    console_clear();
    printf("%s\n", title);
    int n = (int)strlen(title);
    for (int i = 0; i < n; i++) putchar('=');
    putchar('\n');
    putchar('\n');
    printf("%s\n", body);
    console_render();
}

/* Block until any port-1 button is pressed. Returns which buttons. */
static joypad_buttons_t wait_for_any_press(void)
{
    while (1) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if (pressed.raw) return pressed;
        wait_ms(16);
    }
}

static void run_photo_flow(void)
{
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2,
                 GAMMA_NONE, FILTERS_RESAMPLE);

    snapstation_signal_start(SS_PORT);

    for (int i = 0; i < 16; i++) {
        /* Double-render so both framebuffers hold this photo before the
         * core screenshots. */
        draw_photo_frame(i);
        draw_photo_frame(i);
        wait_ms(80);

        snapstation_signal_photo_ready(SS_PORT);

        /* Let the core finish writing the PNG before changing content. */
        wait_ms(150);
    }

    snapstation_signal_end(SS_PORT);

    display_close();
}

int main(void)
{
    /* Bring libdragon's debug channels up early so any assert or
     * debug_printf is routed to the IS-Viewer peripheral. */
    debug_init_isviewer();

    /* joypad_init bootstraps the JoyBus subsystem - without it,
     * snapstation_detect would block the CPU indefinitely waiting on
     * an SI DMA completion interrupt that never fires. */
    joypad_init();

    console_init();
    console_set_render_mode(RENDER_MANUAL);

    while (1) {
        show_console_page(
            "POKEMON SNAP STATION",
            "HLE emulation demo.\n\n"
            "Controller port 4 is the Snap\n"
            "Station printer.\n\n"
            "Press any button on port 1 to\n"
            "begin a print session.");
        wait_for_any_press();

        show_console_page(
            "Detecting Snap Station",
            "Probing port 4 via JoyBus...");
        wait_ms(180);

        if (!snapstation_detect(SS_PORT)) {
            show_console_page(
                "ERROR: station not detected",
                "The input plugin isn't answering\n"
                "JoyBus reads on port 4.\n\n"
                "Ensure mupen64plus is using\n"
                "mupen64plus-input-snapstation.dll.\n\n"
                "Press any button to return.");
            wait_for_any_press();
            continue;
        }

        /* Credit poll: redraw whenever the balance changes so the user
         * sees Minus-press insertions reflected in real time. */
        uint32_t last_shown = (uint32_t)-1;
        int started = 0;
        while (!started) {
            uint32_t bal = snapstation_read_balance(SS_PORT);
            if (bal != last_shown) {
                console_clear();
                printf("CREDIT CHECK\n============\n\n");
                printf("Balance: %u credit%s\n\n",
                       (unsigned)bal, bal == 1 ? "" : "s");
                if (bal > 0) {
                    printf("Press A to start the print flow.\n");
                    printf("(Uses one credit.)\n\n");
                    printf("Press B to cancel.\n");
                } else {
                    printf("No credits available. Press Minus\n");
                    printf("on port 1 to insert a card.\n\n");
                    printf("Press B to cancel.\n");
                }
                console_render();
                last_shown = bal;
            }

            joypad_poll();
            joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
            if (pressed.a && bal > 0) started = 1;
            else if (pressed.b) break;
            else wait_ms(80);
        }
        if (!started) continue;  /* user pressed B - back to splash */

        run_photo_flow();

        /* display_close frees both buffers; re-init console for the
         * post-print screens. The plugin has already opened its
         * Windows print dialog on the host side at this point. */
        console_init();
        console_set_render_mode(RENDER_MANUAL);

        uint32_t bal_after = snapstation_read_balance(SS_PORT);
        char body[320];
        snprintf(body, sizeof(body),
                 "16 frames captured and sent.\n"
                 "The emulator has opened a\n"
                 "Windows print dialog on the\n"
                 "host. Confirm or cancel it\n"
                 "there, then return here.\n\n"
                 "Balance remaining: %u credits.\n\n"
                 "Press any button to return to\n"
                 "the menu.",
                 (unsigned)bal_after);
        show_console_page("PRINT COMPLETE", body);
        wait_for_any_press();
        /* loop back to splash */
    }

    return 0;
}
