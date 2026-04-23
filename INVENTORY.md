# snap-station-emu Inventory (Phase 0)

Snapshot of the repo at the start of the Pokémon Snap Station reproduction
work. This document records what exists on `main`, what we are keeping as
authoritative, what will be extended, and what will be replaced. It does not
move any files: moves happen in the phase that ships the replacement so the
active build never breaks.

See `/root/.claude/plans/ultraplan-v2-pokemon-binary-turtle.md` for the full
multi-repo plan.

## Keep (authoritative; do not rewrite)

| Path | Purpose | Source |
|---|---|---|
| `docs/ROM_SPEC.md` | JoyBus protocol + sticker-sheet geometry spec. | jamchamb 2021, Andrew 2023 measurements (CC0, Internet Archive `slide-1_202310`). |
| `src/joybus_snapstation.c`, `src/joybus_snapstation.h` | JoyBus FE/85 handshake, CC/33/5A trigger, 01/02x16/04 capture state machine. | jamchamb 2021. |
| `src/smart_card.c`, `src/smart_card.h` | PI DOM2 credit-card reader emulator (0xD2 status, 0xE1 ID, 0xA5 read, 0xF0 commit). | Andrew 2023. |
| `src/sticker_sheet.c`, `src/sticker_sheet.h` | 300 DPI hagaki compositor at measured geometry (148x100 mm, 4x4, 26.6x20.0 mm sticker backing, 24.1x17.5 mm kiss-cut, asymmetric 0.833/1.667 mm insets). | Andrew 2023 measurements. |
| `src/stb_image.h` | Vendored PNG/BMP loader (public domain). | stb. |
| `rom/snapstation.h`, `rom/snapstation.c`, `rom/demo/` | ROM-side libdragon driver. Includes HLE-only `SS_ADDR_CREDIT = 0xD000` channel that returns the emulator smart-card balance as big-endian uint32 in bytes[28..31]. | Project-original. |
| `test/test_joybus.c`, `test/test_smart_card.c`, `test/test_sticker_sheet.c`, `test/jstest.c` | Unit tests driving each subsystem headlessly. `make test` already passes. | Project-original. |
| `Makefile` | Linux/macOS test build. Already cross-platform for the test targets. | Project-original. |
| `docs/sticker_sheet_layout_reference.png`, `docs/sticker_sheet_c_output.png`, `docs/sticker_sheet_c_output.bmp` | Visual references for compositor parity checks in the web-app pixel-parity suite. | Project-original. |

## Extend (keep architecture, add capability in a later phase)

| Path | Phase | Planned extension |
|---|---|---|
| `src/m64p_input_plugin.c` (58 KB) | Phase 2 | Evaluate cross-platform readiness. Expected addition: a TCP control socket the web-app CI can drive to trigger JoyBus flows headlessly. Rebuild with SDL2 if any remaining Win32 calls are found. |
| `Makefile` | Phase 1 | Extend with `splat`, `N64Recomp`, and `mips_to_c` targets for the NPHE/NPFE analysis pipeline. Keep existing test targets intact. |
| `docs/ROM_SPEC.md` | Phase 2 | Append NPHE-vs-NPFE divergence notes as they are discovered via splat + Ghidra. |
| `rom/snapstation.h` | Phase 2 | Add any libdragon driver entry points required by the behavioral reference harness. `SS_ADDR_CREDIT` stays as-is. |

## Replace (archive in a later phase when the replacement lands)

These remain on `main` for now so the Windows build path is not broken
mid-flight. When their cross-platform replacements land, they move to
`archive/` with a short note in that phase's commit message.

| Path | Replaced by | Phase |
|---|---|---|
| `src/snap_station_win32.c` | Cross-platform print-dialog bridge (CUPS on Linux/macOS, Win32 spooler on Windows) in `src/print_bridge_*.c`. | Phase 2 |
| `build_win.bat`, `build_dev.bat`, `build_dll_verbose.bat` | CMake build (`emu/build/CMakeLists.txt`) driving MSYS2/Linux/macOS uniformly. | Phase 1 |

## Context not in the repo but in scope

- Retail Pokémon Snap NPFE US ROM: available to the project, used as
  ground truth via `pmret/pokemonsnap` decomp submodule in Phase 1.
- Kiosk Pokémon Snap NPHE US ROM: available, the primary target for
  `N64Recomp` static recompilation in Phase 1.
- No EUR/JPN variants are in scope for v1.

## Open questions surfaced during inventory

None. Geometry, protocol, and licensing are all cited in `docs/ROM_SPEC.md`.
