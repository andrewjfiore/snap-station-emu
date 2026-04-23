# emu/tools

Bootstrap scripts for the N64 analysis pipeline.

## First-time setup

```
git submodule update --init --recursive
python3 -m pip install -r tools/splat/requirements.txt
```

## Validate a ROM dump

```
python emu/tools/extract_rom.py /path/to/snap_retail.bin npfe
python emu/tools/extract_rom.py /path/to/snap_kiosk.bin nphe
```

On first run per variant the script prints the observed SHA-1 and
exits non-zero. Pin the hash into `EXPECTED_SHA1` at the top of
`extract_rom.py`, commit, and re-run. Every subsequent run verifies.

## Split a validated ROM

```
python tools/splat/split.py emu/config/npfe.yaml
python tools/splat/split.py emu/config/nphe.yaml
```

Output lands under `build/{npfe,nphe}/` with `asm/`, `src/`, `assets/`
siblings per the config.

## Build the test harness

```
cmake -S emu/build -B build/cmake
cmake --build build/cmake
ctest --test-dir build/cmake
```

The top-level `Makefile` still works on Linux/macOS for the same
tests; CMake adds Windows (MSVC/MSYS2) coverage and the optional
Mupen64Plus input plugin target (`-DWITH_M64P_PLUGIN=ON`).
