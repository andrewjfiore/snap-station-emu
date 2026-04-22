# Snap Station ROM Specification

This document consolidates everything a "novel ROM for printing" needs to
know to drive a real Snap Station kiosk (or our station simulator) end to
end. It is derived from:

* jamchamb's 2021 reverse engineering of retail Pokemon Snap
  (https://jamchamb.net/2021/08/17/snap-station.html)
* Andrew's 2023 sticker-sheet dimension measurements
  (Internet Archive item `slide-1_202310`, CC0)

We have no access to a ROM dump of retail Pokemon Snap. Everything here
is the minimum the ROM running on the N64 needs to do, not what retail
Snap actually does.

---

## 1. Bus topology

The Snap Station is a **controller with a non-standard peripheral**,
plugged into **N64 controller port 4**. It uses the JoyBus protocol.
All exchanges use two Controller-Pak commands:

| Command | Send length | Receive length | Meaning |
|---|---|---|---|
| `0x02` | 3 bytes (cmd + 16-bit addr with CRC5) | 33 bytes (32 data + CRC8) | Pak READ |
| `0x03` | 35 bytes (cmd + addr + 32 data) | 1 byte (CRC8) | Pak WRITE |

The address field is **not a memory address**. It is a message channel.
Two channels matter:

* `0x8000` - peripheral identification / handshake
* `0xC000` - print-flow state machine

Every other address is ignored by the station.

## 2. Peripheral-ID handshake (address 0x8000)

Before anything else, the ROM confirms a Snap Station is actually
connected, not a Rumble Pak or Controller Pak.

```
ROM -> station:  WRITE 0x8000, 32x 0xFE    (reset probe)
station -> ROM:  ACK with status 0xE1      (returned in write response)

ROM -> station:  READ  0x8000
station -> ROM:  32x 0x00 + CRC8

ROM -> station:  WRITE 0x8000, 32x 0x85    (ID probe for Snap Station pak)
station -> ROM:  ACK with status 0xF5

ROM -> station:  READ  0x8000
station -> ROM:  32x 0x85 + CRC8           (confirms Snap Station)
```

Pak ID table:
* `0x80` - Rumble Pak
* `0x85` - Snap Station
* Controller Pak identifies via the CRC on the pak-probe command itself
  and does not echo 0x85.

If this handshake succeeds during the N64 boot splash (before the title
screen), retail Pokemon Snap jumps to photo-display mode. If it succeeds
later, it enables the Print button in the Gallery menu.

## 3. Print-flow state channel (address 0xC000)

Only the **last byte of each 32-byte write** matters. All preceding
bytes are zero-padding. On reads, the station returns the same trailing
byte, or `0x08` to stall the ROM in a busy loop.

### 3a. Gallery "Print" pressed

```
ROM -> station:  WRITE 0xC000, ...00 00 CC   (pre-save)
ROM -> station:  WRITE 0xC000, ...00 00 33   (post-save)
ROM -> station:  WRITE 0xC000, ...00 00 5A   (please reset console)
```

Station returns `0x08` busy on reads after `0x5A` until the console
actually resets. This keeps the "Now Saving..." UI on screen.

### 3b. Photo display mode (entered after reset)

```
ROM -> station:  WRITE 0xC000, ...00 00 01   (start)
for i in 0..15:
    ROM displays photo i fullscreen
    ROM -> station:  WRITE 0xC000, ...00 00 02   (one per photo)
    ROM waits while station returns 0x08 busy (camera capture in progress)
ROM -> station:  WRITE 0xC000, ...00 00 04   (end)
```

The printer captures each photo from the N64's **video output** via a
pass-through connection in the station. No pixel data ever crosses the
JoyBus. The `0x02` signal tells the station "this frame is ready to be
captured, hold it."

## 4. Sticker-sheet geometry (from Andrew's measurements)

```
Paper                    148.0 mm x 100.0 mm   (Japanese hagaki postcard)
Print area               109.4 mm x  83.0 mm
Grid                     4 cols x 4 rows = 16 stickers
Sticker backing          26.6 mm x 20.0 mm     (each)
Visible sticker          24.1 mm x 17.5 mm     (after kiss-cut)
Corner radius            2.75 mm
Cut inset top            0.833 mm
Cut inset bottom         1.667 mm              (asymmetric - label zone)
Cut inset left           1.25 mm
Cut inset right          1.25 mm
Gutter horizontal        1.0 mm
Gutter vertical          1.0 mm
```

Verification:
* 4 x 26.6 + 3 x 1.0 = 109.4 mm  (matches print-area width)
* 4 x 20.0 + 3 x 1.0 =  83.0 mm  (matches print-area height)

The sticker aspect ratio is **24.1 : 17.5 = 1.377**, slightly wider than
NTSC 4:3 (1.333). The ROM should letterbox N64 output by cropping ~7%
off the top and bottom, OR accept a small amount of stretch on print.

Because the printer captures live video, the ROM's job is to render each
photo with:
* Full screen clear to black or a border color
* Photo centered, at N64 native resolution
* Held on screen long enough for the pass-through to capture
  (the `0x08` busy response governs this timing)

## 5. ROM requirements summary

A "novel printing ROM" must:

1. Initialize the N64 video mode (320x240 NTSC, 16-bit direct color is
   fine; matches retail Pokemon Snap native output).
2. Poll port 4 for a Snap Station, performing the 0x8000 handshake.
   If absent, degrade gracefully (show error, return to menu).
3. Provide some source of 16 photos. For a demo this can be
   compile-time embedded or procedurally generated; for a real app this
   would be a camera/gallery UI.
4. Run the Gallery print trigger (CC, 33, 5A).
5. Either wait for a real reset or synthesize the post-reset state
   directly (the station will busy-loop us either way).
6. Run photo display mode (01, 16x 02, 04), displaying one photo
   per 02 signal.
7. The ROM does **not** composite stickers. The station's printer does
   that mechanically via paper feed and the video-capture pipeline.
   The ROM just presents frames.

## 6. Open questions

* **Exact 02 timing.** jamchamb notes the station can hold the ROM via
  `0x08` busy; we do not know how long the real printer needs. Our
  station simulator holds ~0.5 seconds per photo as a reasonable guess.
* **What happens on read from 0xC000 pre-handshake?** jamchamb's code
  doesn't show this edge case. We assume 32 zero bytes.
* **Does the station check the entire 32 bytes written or only the
  last byte?** jamchamb observed only the last byte matters for the
  state machine, so our ROM zero-pads.
* **Are photos displayed at 320x240 exactly, or padded?** Unknown; we
  go with 320x240 fullscreen, black-padded if the photo data is
  smaller.
