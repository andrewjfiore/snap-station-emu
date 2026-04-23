#!/usr/bin/env python3
"""Drive a YAML scenario against one of two backends and emit frame hashes.

Usage:
    python emu/harness/run_script.py <scenario.yaml> --track=ares
    python emu/harness/run_script.py <scenario.yaml> --track=recomp
    python emu/harness/run_script.py <scenario.yaml> --track=both

When --track=both, ares and recomp run concurrently; the script
hard-fails if any labeled hash differs between tracks. This is the
invariant the plan calls the `two-track validation`.

Backends are shelled out as subprocesses; their exact invocation is
configured per scenario so contributors can point at a local ares build
or a N64Recomp-produced binary without editing this script. The backend
is expected to stream newline-delimited JSON events on stdout of the form
  {"label": "...", "frame_sha256": "..."}
at each scripted capture point. Non-JSON lines on stdout or any output
on stderr are relayed to the harness's stderr verbatim.

Frame hashes stored in the scenario YAML as null are pinned on the first
successful run with --pin (same pattern as emu/tools/extract_rom.py).
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore
except ImportError:  # pragma: no cover - bootstrap guidance
    print("PyYAML missing. Install with: pip install pyyaml", file=sys.stderr)
    sys.exit(2)


DEFAULT_TIMEOUT_S = 60


def run_backend(cmd: list[str], env: dict[str, str], timeout_s: int) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,  # merge so a single reader drains both
        env={**os.environ, **env},
        text=True,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.strip()
        if not line:
            continue
        if line.startswith("{"):
            try:
                events.append(json.loads(line))
                continue
            except json.JSONDecodeError:
                pass
        # Non-JSON: relay as diagnostic; backends are noisy.
        print(f"[{cmd[0]}] {line}", file=sys.stderr)
    rc = proc.wait(timeout=timeout_s)
    if rc != 0:
        raise RuntimeError(f"backend exited {rc}: {cmd}")
    return events


def diff_tracks(tracks: dict[str, list[dict[str, Any]]]) -> list[str]:
    errors: list[str] = []
    names = sorted(tracks.keys())
    if len(names) < 2:
        return errors
    by_label = {n: {e.get("label"): e for e in tracks[n] if "label" in e} for n in names}
    all_labels = sorted(set().union(*(by_label[n].keys() for n in names)))
    for label in all_labels:
        hashes = {n: by_label[n].get(label, {}).get("frame_sha256") for n in names}
        if None in hashes.values():
            missing = [n for n, h in hashes.items() if h is None]
            errors.append(f"label {label!r} missing on: {missing}")
            continue
        if len(set(hashes.values())) > 1:
            errors.append(f"label {label!r} hash mismatch: {hashes}")
    return errors


def validate_against_pins(events: list[dict[str, Any]],
                          scenario: dict[str, Any]) -> tuple[list[str], bool]:
    errors: list[str] = []
    updated = False
    expected = scenario.setdefault("expected_hashes", {})
    for ev in events:
        label = ev.get("label")
        if label is None:
            continue
        observed = ev.get("frame_sha256")
        pinned = expected.get(label)
        if pinned is None:
            expected[label] = observed
            updated = True
            print(f"pin {label}={observed}")
        elif pinned != observed:
            errors.append(f"{label}: expected {pinned}, observed {observed}")
    return errors, updated


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario")
    parser.add_argument("--track", choices=["ares", "recomp", "both"], default="both")
    parser.add_argument("--pin", action="store_true",
                        help="Rewrite scenario YAML with newly observed hashes. Requires --track=both.")
    args = parser.parse_args()

    if args.pin and args.track != "both":
        print("--pin requires --track=both so both backends agree before committing hashes",
              file=sys.stderr)
        return 2

    path = Path(args.scenario)
    scenario = yaml.safe_load(path.read_text())
    backends = scenario.get("backends", {})
    timeout_s = int(scenario.get("timeout", DEFAULT_TIMEOUT_S))

    names = ["ares", "recomp"] if args.track == "both" else [args.track]
    for name in names:
        if backends.get(name) is None:
            print(f"scenario missing backends.{name}", file=sys.stderr)
            return 2

    all_events: dict[str, list[dict[str, Any]]] = {}
    with ThreadPoolExecutor(max_workers=len(names)) as pool:
        futures = {
            name: pool.submit(
                run_backend,
                backends[name]["cmd"],
                backends[name].get("env", {}),
                timeout_s,
            )
            for name in names
        }
        for name, fut in futures.items():
            all_events[name] = fut.result()

    errors: list[str] = []
    cross_errors = diff_tracks(all_events) if args.track == "both" else []
    for name, events in all_events.items():
        errs, _updated = validate_against_pins(events, scenario)
        errors.extend(f"{name}: {e}" for e in errs)
    errors.extend(f"cross-track: {e}" for e in cross_errors)

    if args.pin and not errors:
        path.write_text(yaml.safe_dump(scenario, sort_keys=False))
        print(f"updated {path} with new pins")

    if errors:
        for e in errors:
            print(f"FAIL {e}", file=sys.stderr)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
