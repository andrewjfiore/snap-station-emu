/* snapstation.h
 *
 * ROM-side driver for the Pokemon Snap Station. Implements jamchamb's
 * JoyBus protocol on top of libdragon's controller-pak primitives.
 *
 * Usage:
 *   controller_init();
 *   if (!snapstation_detect(SS_PORT)) { error }
 *   if (!snapstation_trigger_print(SS_PORT)) { error }
 *   for (int i = 0; i < 16; i++) {
 *       render_photo(i);
 *       video_present();
 *       snapstation_signal_photo_ready(SS_PORT);
 *   }
 *   snapstation_signal_end(SS_PORT);
 */

#ifndef SNAPSTATION_H
#define SNAPSTATION_H

#include <stdint.h>
#include <stdbool.h>

/* Real stations sit on controller port 4. The constant is 0-based in
 * libdragon, so port 4 == index 3. */
#define SS_PORT        3

/* JoyBus message addresses (not memory addresses; message channels). */
#define SS_ADDR_ID     0x8000u
#define SS_ADDR_FLOW   0xC000u

/* Peripheral ID bytes. */
#define SS_PAK_ID      0x85u   /* Snap Station  */
#define SS_RUMBLE_ID   0x80u   /* Rumble Pak    */

/* Flow-channel byte patterns. */
#define SS_FLOW_PRE_SAVE     0xCCu
#define SS_FLOW_POST_SAVE    0x33u
#define SS_FLOW_REQ_RESET    0x5Au
#define SS_FLOW_DISPLAY_BEGIN 0x01u
#define SS_FLOW_DISPLAY_PHOTO 0x02u
#define SS_FLOW_DISPLAY_END   0x04u
#define SS_FLOW_BUSY          0x08u   /* station returns this to stall the ROM */

/* HLE extension: the mupen64plus Snap Station input plugin surfaces the
 * emulator's smart-card credit balance on this JoyBus channel. Not part
 * of real Snap Station hardware - it's an emulator-side convenience so
 * ROMs can gate printing on credits without needing a PI DOM2 core hook.
 * Reads return a 32-byte block with the balance as a big-endian uint32
 * in bytes[28..31]. */
#define SS_ADDR_CREDIT        0xD000u

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the given port for a Snap Station. Performs the FE/85 handshake
 * at 0x8000 and confirms the station echoes back 0x85. Returns true if
 * a station was detected. Safe to call at any time - does not leave the
 * station in a modified state. */
bool snapstation_detect(int port);

/* Send the print-trigger sequence (CC, 33, 5A). After 0x5A the station
 * should busy-loop, conventionally by cycling the console. Our
 * simulator short-circuits this; a real station requires a real reset.
 * Returns true if all three writes were acknowledged. */
bool snapstation_trigger_print(int port);

/* Send the photo-display start byte (0x01). */
bool snapstation_signal_start(int port);

/* Signal that the currently displayed frame is ready for capture
 * (0x02). Polls the station until it releases the busy flag. Intended
 * to be called once per displayed photo; call between 16 frame swaps
 * in the 4x4 grid. */
bool snapstation_signal_photo_ready(int port);

/* Send the photo-display end byte (0x04). After this the station
 * physically prints the sticker sheet. */
bool snapstation_signal_end(int port);

/* Read the current flow-channel status byte without advancing the state
 * machine. Returns SS_FLOW_BUSY, 0x00, or whatever the station last
 * latched. */
uint8_t snapstation_peek_flow(int port);

/* HLE extension: read the emulator-side smart-card credit balance via
 * the SS_ADDR_CREDIT channel. Returns 0 on read error or when no card
 * is inserted. Not a real Snap Station hardware feature - see the note
 * on SS_ADDR_CREDIT above. */
uint32_t snapstation_read_balance(int port);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSTATION_H */
