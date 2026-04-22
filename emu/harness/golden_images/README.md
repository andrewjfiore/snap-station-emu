# Golden frame dumps

This directory holds SHA-256-pinned frame captures at each labeled point
in the scenarios under `emu/harness/scenarios/`. Contents are populated
on first successful run of `run_script.py --pin` by the contributor with
access to the ROMs. Filenames follow `<scenario>_<label>.png` with a
sibling `.sha256` text file for CI consumption.

Do **not** commit raw ROM dumps or copyright-sensitive in-game imagery.
These golden images are for pixel-parity regression only; prefer UI
chrome and menu screens over gameplay footage. If a scenario label
requires a copyrighted frame, store only its hash and fetch the
reference lazily at test time from a local artifact store.
