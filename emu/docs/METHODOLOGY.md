# N64 Analysis Methodology

This document is the standing reference for how we analyze, decompile,
and cross-validate the NPHE (kiosk) and NPFE (retail) Pokemon Snap ROMs.
It exists so contributors do not re-derive the workflow each time.

## Two-track validation

Every behavioral claim about the original kiosk must be reproducible
on **both** of these independent tracks, and the two must agree:

1. **Dynamic track - `ares`.** Reference N64 emulator (byuu/ares). Run
   the ROM headlessly, script controller input via ares's automation,
   dump frame hashes and JoyBus traffic. Source of truth for timing.
2. **Static track - `N64Recomp`.** Mr-Wiseguy's static recompiler.
   Convert the ROM to native x86/ARM, link against our stubs for
   libultra peripherals, run the same scenarios. Source of truth for
   control flow.

Divergence between tracks at any labeled scenario point in
`emu/harness/scenarios/*.yaml` is a hard CI failure. Resolve before
merging.

## Splitting and decompilation

- `tools/splat` splits the ROM into header / boot / code / data / assets
  per `emu/config/{nphe,npfe}.yaml`.
- `tools/pokemonsnap-reference` (pmret/pokemonsnap) is the authoritative
  NPFE baseline. Prefer its config and symbol tables when they exist.
- `tools/mips_to_c` produces first-pass C for functions we want to
  match on decomp.me. Do not commit its raw output; commit the matched,
  hand-cleaned version.
- `tools/crunch64` handles Yaz0/MIO0 decompression of any compressed
  asset blobs found during splat boundary detection.
- Ghidra (external, not vendored) with the [N64 loader plugin]
  (https://github.com/zeroKilo/N64LoaderWV) and the community function-ID
  database gives us initial function naming. Export symbols back into
  `symbols_{nphe,npfe}.txt` for splat to consume.

## NPHE-specific notes

- No public decomp exists. Start by cross-referencing NPFE symbols.
- The protocol described in `docs/ROM_SPEC.md` is the contract. Any
  NPHE function that touches JoyBus port 4 or PI DOM2 at 0x08000000
  should be named and linked to a ROM_SPEC section.
- The HLE-only credit channel at `SS_ADDR_CREDIT = 0xD000` is an
  emulator convenience (see `rom/snapstation.h`); NPHE itself uses the
  real PI DOM2 smart-card reader.

## Decomp.me workflow

For every function we want to match:

1. Extract the assembly from splat's output under `asm/{nphe,npfe}/`.
2. Paste into a decomp.me scratch with compiler preset `IDO 5.3 -O2`
   (same preset as pmret/pokemonsnap).
3. Iterate until the match score is 100% or the diff is empty, then
   commit under `src/{nphe,npfe}/` and mark the function done in the
   splat symbol file.

## HD rendering path (exploratory, not blocking)

`RT64` (Mr-Wiseguy) can render N64Recomp output at HD with ray-traced
lighting. We do not depend on this for behavioral validation; it is a
future path for the kiosk's attract mode.

## What this methodology deliberately does not do

- No dynamic recompilation at runtime (too expensive for kiosk host
  hardware). Static AOT only.
- No HLE graphics for validation. We compare frame hashes captured
  from the reference emulator; any divergence in an HLE renderer is
  noise for our purposes.
- No kiosk-modification path. We reproduce the kiosk's behavior, we
  do not patch or crack the original ROMs.
