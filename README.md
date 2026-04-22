# Snap Station Emulation for Mupen64Plus

Emulates the Pokemon Snap Station kiosk hardware so a ROM running under
Mupen64Plus can trigger a real print job through the standard Windows
printer dialog. The output is a 4x4 sticker sheet laid out on a
148x100 mm Japanese hagaki postcard, matching the dimensions of real
retail sheets.

```
N64 ROM (retail Snap, kiosk NPHE, or a novel ROM using rom/snapstation.h)
  |
  | Controller port 4 (JoyBus)         Cart bus (PI DOM2)
  |       |                                    |
  v       v                                    v
+---------------------------+      +------------------------+
| joybus_snapstation.c      |      | smart_card.c           |
| FE/85 handshake           |      | Credit card reader     |
| CC/33/5A trigger          |      | 0xE1 device id         |
| 01 / 02x16 / 04 capture   |      | 0xA5 read, 0xF0 commit |
+------------+--------------+      +-----------+------------+
             |                                  |
             v                                  v
   +------------------------+    +----------------------------+
   | sticker_sheet.c        |    | snap_station_win32.c       |
   | Compose 148x100 mm     |<---| Screenshot via core cmd    |
   | hagaki sheet at 300DPI |    | Open Windows print dialog  |
   +------------------------+    +----------------------------+
                                            |
                                            v
                                   Any installed printer
```

---

## What's in the box

```
snap_station_emu/
|-- README.md                       (this file)
|-- Makefile                        Linux/macOS tests
|-- build_win.bat                   Windows build (MSVC)
|-- src/
|   |-- sticker_sheet.h/c           Compositor at your exact dimensions
|   |-- joybus_snapstation.h/c      Printer protocol (jamchamb's work)
|   |-- smart_card.h/c              Credit-card reader emulator
|   |-- snap_station_win32.c        Windows print-dialog bridge
|   `-- m64p_input_plugin.c         Mupen64Plus input plugin (port 4)
|-- test/
|   |-- test_sticker_sheet.c        Renders a demo BMP
|   |-- test_joybus.c               Drives the full JoyBus flow
|   `-- test_smart_card.c           Exercises card insert/commit
|-- docs/
|   |-- ROM_SPEC.md                 Full protocol spec
|   |-- sticker_sheet_layout_reference.png    Visual reference
|   `-- sticker_sheet_c_output.{bmp,png}      Compositor output
`-- rom/
    `-- snapstation.h               ROM-side driver header
```

---

## Protocol at a glance

The Snap Station is **not** on the cartridge bus. jamchamb's 2021
reverse engineering established that it presents as a controller with
a pak (ID 0x85) plugged into **port 4**, speaking the JoyBus
Controller-Pak read/write commands with the address field repurposed
as a message channel:

| Channel | Purpose |
|---|---|
| `0x8000` | Peripheral identification (FE reset, 85 pak-ID probe) |
| `0xC000` | Print flow state machine |

Print flow:
1. Gallery Print pressed: ROM writes `0xCC`, `0x33`, `0x5A` to 0xC000
2. Station returns `0x08` busy after `0x5A` until console resets
3. On boot, ROM re-detects station, enters photo display mode
4. ROM writes `0x01` (start), sixteen `0x02` (one per photo), `0x04` (end)
5. Station's printer captures each frame from the N64's video output

The printer never receives pixel data over any bus. It captures each
of 16 photos from a video pass-through on the kiosk chassis; the
`0x02` signal tells the printer "this frame is ready, snap it."

The PI DOM2 device at physical `0x08000000` is **separate** - it's the
smart-card reader that holds the player's print credits. The NPHE
kiosk ROM talks to it via libultra's `osEPiWriteIo`/`osEPiReadIo` with
a custom opcode set (`0xD2` status, `0xE1` device ID, `0xA5` read,
`0xF0` commit). Our `smart_card.c` emulates it: insert a card with
N credits, each commit transaction decrements the balance.

See `docs/ROM_SPEC.md` for the full protocol including exact byte
sequences from jamchamb's captures.

---

## Sticker sheet geometry

From measurements of a real retail sheet (Andrew's 2023 capture,
Internet Archive CC0):

```
Paper                148.0 x 100.0 mm   Japanese hagaki postcard
Print area           109.4 x  83.0 mm
Grid                 4 cols x 4 rows = 16 stickers
Sticker backing       26.6 x  20.0 mm   (each cell)
Kiss-cut visible      24.1 x  17.5 mm   (rounded corners)
Corner radius          2.75 mm
Insets  top            0.833 mm
Insets  bottom         1.667 mm         (asymmetric label zone)
Insets  side           1.25 mm
Gutters H + V          1.0 mm
```

Geometry verified:
* `4 x 26.6 + 3 x 1.0 = 109.4 mm` (print width) OK
* `4 x 20.0 + 3 x 1.0 =  83.0 mm` (print height) OK
* `2 x 1.25 + 24.1    =  26.6 mm` (backing width) OK
* `0.833 + 17.5 + 1.667 = 20.0 mm` (backing height) OK

At the default 300 DPI: sheet = **1748 x 1181 px**, kiss-cut = 285 x 207 px.

The compositor scales each captured photo to fill the kiss-cut rect
(crop-fill by default, configurable to letterbox). N64 native 4:3 and
the kiss-cut 1.377:1 aspect differ slightly (~3% top-and-bottom crop).

---

## Quick start: standalone tests (no emulator needed)

### Linux / macOS

```
make test
```

All three tests should print `RESULT: PASS` (or equivalent). The
sticker-sheet test also writes `sticker_sheet_demo.bmp` to the current
directory - open it to see 16 synthetic gradient photos composed on
the exact hagaki layout.

### Windows

Open an **x64 Native Tools Command Prompt for VS 2022**:

```
build_win.bat
test_sticker_sheet.exe && test_joybus.exe && test_smart_card.exe
```

---

## Mupen64Plus integration

The Snap Station printer runs as a **Mupen64Plus input plugin** that
owns controller port 4. No core patch required.

### Install

1. Build on Windows (`build_win.bat`) to produce
   `mupen64plus-input-snapstation.dll`.
2. Copy it next to your `mupen64plus.dll` / `mupen64plus-ui-console.exe`
   (the same folder as the stock bundle).
3. Edit `mupen64plus.cfg`:
   ```
   [UI-Console]
   InputPlugin = "mupen64plus-input-snapstation.dll"
   ```
4. Launch a ROM that implements the Snap Station protocol.

### Running

* Ports 1-3 are inert under this plugin. If you need a regular gamepad
  for the ROM, the cleanest path is to edit the plugin source to chain
  `mupen64plus-input-sdl.dll` for ports 1-3 and handle only port 4
  yourself, or use an external controller API.
* When the ROM triggers `0x02` on the 0xC000 channel, the plugin calls
  `CoreDoCommand(M64CMD_TAKE_NEXT_SCREENSHOT)` which writes a PNG to
  Mupen64Plus's configured screenshot directory
  (`%APPDATA%\Mupen64Plus\screenshot\` by default).
* After sixteen `0x02` events and one `0x04`, the plugin loads the 16
  newest files in the screenshot directory, composes them via
  `sticker_sheet.c`, saves a BMP to
  `%USERPROFILE%\Documents\SnapStation\`, and opens the standard
  Windows print dialog.

### Smart-card emulation

The PI DOM2 credit reader emulation is **not** loaded by the input
plugin (the plugin is just the JoyBus side). Two options:

1. **Standalone testing**: run `test_smart_card.exe` to exercise the
   reader protocol without Mupen64Plus.
2. **Core integration**: apply the patch in `patches/smart_card_hook.patch`
   (not yet written - requires mupen64plus-core source modification
   following the pattern described in `patches/README`).

If you're only running a ROM that skips credit checks (e.g., a novel
printing ROM using `rom/snapstation.h`), you can ignore smart-card
emulation entirely.

---

## Caveats

* **ROM compatibility.** Per jamchamb, the actual printing protocol
  lives in the retail Pokemon Snap ROM (NPFE), not in the NPHE kiosk
  dump. For a working end-to-end demo you need either (a) a retail
  Pokemon Snap cart dump, or (b) a custom ROM using our
  `rom/snapstation.h` driver.
* **HLE RSP.** Mupen64Plus 2.6.0's HLE RSP plugin does not render all
  HAL-engine games perfectly. Use `mupen64plus-rsp-cxd4` (LLE) if the
  ROM misbehaves.
* **Screenshot format.** Mupen64Plus writes PNG; our Win32 bridge
  currently loads BMPs. Until a PNG loader is wired in (use libpng or
  stb_image), test with a video plugin that outputs BMP, or patch the
  screenshot format. The test harness uses BMP end-to-end so the
  protocol/compositor pipeline is fully exercised.
* **The "printer" code in NPHE.** Earlier iterations of this project
  mis-identified the PI DOM2 peripheral in the NPHE ROM as a printer.
  It is the smart-card reader. jamchamb's work is the authority on
  the actual printer protocol.

---

## Credits

* **jamchamb** - 2021 reverse engineering of the Snap Station protocol
  via JoyBus probing with an iCEBreaker FPGA.
  <https://jamchamb.net/2021/08/17/snap-station.html>
* **Andrew Fiore** - 2023 measurement of real retail sticker-sheet
  dimensions, released CC0 via Internet Archive item `slide-1_202310`.

## License

Public domain (CC0). The Mupen64Plus core this plugin runs inside is
GPLv2; if you distribute a combined binary, GPLv2 applies.
