/* snapstation.c
 *
 * libdragon implementation of the Snap Station ROM-side driver declared
 * in snapstation.h. Uses libdragon's joybus_accessory_read/write as the
 * transport; our mupen64plus input plugin decodes these in
 * joybus_snapstation.c on the emulator side.
 *
 * This driver also handles the HLE credit extension (SS_ADDR_CREDIT):
 * the plugin synthesises a balance response on that channel so custom
 * ROMs can gate printing on credits without a PI DOM2 core patch.
 */

#include "snapstation.h"

#include <libdragon.h>
#include <string.h>

/* Fill a 32-byte pak block whose only meaningful byte is the trailing
 * signal. jamchamb's capture of the real Snap Station shows the station
 * inspects only this byte; everything else is padding. */
static bool write_trail(int port, uint16_t addr, uint8_t trail)
{
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    buf[31] = trail;
    return joybus_accessory_write(port, addr, buf)
           == JOYBUS_ACCESSORY_IO_STATUS_OK;
}

static uint8_t read_trail(int port, uint16_t addr)
{
    uint8_t buf[32];
    if (joybus_accessory_read(port, addr, buf)
        != JOYBUS_ACCESSORY_IO_STATUS_OK) {
        return 0xFF;
    }
    return buf[31];
}

bool snapstation_detect(int port)
{
    /* Real Snap Station responds to an FE probe with FE, then to an 85
     * probe with 85. That's the entire identification handshake. */
    if (!write_trail(port, SS_ADDR_ID, 0xFE))      return false;
    if (read_trail(port, SS_ADDR_ID) != 0xFE)      return false;

    if (!write_trail(port, SS_ADDR_ID, SS_PAK_ID)) return false;
    if (read_trail(port, SS_ADDR_ID) != SS_PAK_ID) return false;

    return true;
}

bool snapstation_trigger_print(int port)
{
    /* CC -> 33 -> 5A. After 5A the station goes busy and real hardware
     * expects a console reset. Our emulator's simulator short-circuits
     * through AWAIT_RESET once the backend handles on_request_reset. */
    if (!write_trail(port, SS_ADDR_FLOW, SS_FLOW_PRE_SAVE))  return false;
    if (!write_trail(port, SS_ADDR_FLOW, SS_FLOW_POST_SAVE)) return false;
    if (!write_trail(port, SS_ADDR_FLOW, SS_FLOW_REQ_RESET)) return false;
    return true;
}

bool snapstation_signal_start(int port)
{
    return write_trail(port, SS_ADDR_FLOW, SS_FLOW_DISPLAY_BEGIN);
}

bool snapstation_signal_photo_ready(int port)
{
    if (!write_trail(port, SS_ADDR_FLOW, SS_FLOW_DISPLAY_PHOTO)) return false;

    /* Poll until the station releases the busy flag. Emulator returns
     * 0x08 for ~30 frames after 0x02 by default (see jbs_capture_stall);
     * 600 tries @ 17 ms ~= 10 s is plenty of headroom. */
    for (int tries = 0; tries < 600; tries++) {
        uint8_t t = read_trail(port, SS_ADDR_FLOW);
        if (t != SS_FLOW_BUSY) return true;
        wait_ms(17);
    }
    return false;
}

bool snapstation_signal_end(int port)
{
    return write_trail(port, SS_ADDR_FLOW, SS_FLOW_DISPLAY_END);
}

uint8_t snapstation_peek_flow(int port)
{
    return read_trail(port, SS_ADDR_FLOW);
}

uint32_t snapstation_read_balance(int port)
{
    uint8_t buf[32];
    if (joybus_accessory_read(port, SS_ADDR_CREDIT, buf)
        != JOYBUS_ACCESSORY_IO_STATUS_OK) {
        return 0;
    }
    return ((uint32_t)buf[28] << 24)
         | ((uint32_t)buf[29] << 16)
         | ((uint32_t)buf[30] <<  8)
         | ((uint32_t)buf[31]);
}
