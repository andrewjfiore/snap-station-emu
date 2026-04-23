#!/usr/bin/env python3
"""Drive a YAML scenario against one of two backends and emit frame hashes.

Usage:
    python emu/harness/run_script.py <scenario.yaml> --track=ares
    python emu/harness/run_script.py <scenario.yaml> --track=recomp
    python emu/harness/run_script.py <scenario.yaml> --track=both

When --track=both, the script runs each backend in turn and hard-fails if
any labeled hash differs between tracks. This is the invariant the plan
calls the `two-track validation`.

Backends are shelled out as subprocesses; their exact invocation is
configured per scenario so contributors can point at a local ares build
or a N64Recomp-produced binary without editing this script. The backend
is expected to stream newline-delimited JSON events on stdout of the form
  {"label": "...", "frame_sha256": "..."}
at each scripted capture point.

Frame hashes stored in the scenario YAML as null are pinned on the first
successful run (same pattern as emu/tools/extract_rom.py). Mismatches
against a pinned hash are a hard failure.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore
except ImportError:  # pragma: no cover - bootstrap guidance
    print("PyYAML missing. Install with: pip install pyyaml", file=sys.stderr)
    sys.exit(2)


def run_backend(cmd: list[str], env: dict[str, str]) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, **env},
        text=True,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.strip()
        if not line:
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            # Backends may log human-readable diagnostics; ignore.
            continue
    rc = proc.wait(timeout=60)
    if rc != 0:
        stderr = proc.stderr.read() if proc.stderr else ""
        raise RuntimeError(f"backend exited {rc}: {cmd}\n{stderr}")
    return events


def diff_tracks(a: list[dict[str, Any]], b: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    by_label_a = {e.get("label"): e for e in a if "label" in e}
    by_label_b = {e.get("label"): e for e in b if "label" in e}
    for label in sorted(set(by_label_a) | set(by_label_b)):
        ea, eb = by_label_a.get(label), by_label_b.get(label)
        if ea is None or eb is None:
            errors.append(f"label {label!r} only present on one track")
            continue
        ha = ea.get("frame_sha256")
        hb = eb.get("frame_sha256")
        if ha != hb:
            errors.append(f"label {label!r} hash mismatch: ares={ha} recomp={hb}")
    return errors


def validate_against_pins(events: list[dict[str, Any]],
                          scenario: dict[str, Any]) -> tuple[list[str], bool]:
    """Compare events to the scenario's expected_hashes. Returns (errors,
    updated) where `updated` is True if any null pin was filled in."""
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
            errors.append(
                f"{label}: expected {pinned}, observed {observed}"
            )
    return errors, updated


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario")
    parser.add_argument("--track", choices=["ares", "recomp", "both"],
                        default="both")
    parser.add_argument("--pin", action="store_true",
                        help="Rewrite scenario YAML with newly observed hashes.")
    args = parser.parse_args()

    path = Path(args.scenario)
    scenario = yaml.safe_load(path.read_text())
    backends = scenario.get("backends", {})

    all_events: dict[str, list[dict[str, Any]]] = {}
    for name in (["ares", "recomp"] if args.track == "both" else [args.track]):
        cfg = backends.get(name)
        if cfg is None:
            print(f"scenario missing backends.{name}", file=sys.stderr)
            return 2
        all_events[name] = run_backend(cfg["cmd"], cfg.get("env", {}))

    errors: list[str] = []
    updated_any = False
    for name, events in all_events.items():
        errs, updated = validate_against_pins(events, scenario)
        errors.extend(f"{name}: {e}" for e in errs)
        updated_any = updated_any or updated

    if args.track == "both":
        errors.extend(
            f"cross-track: {e}"
            for e in diff_tracks(all_events["ares"], all_events["recomp"])
        )

    if updated_any and args.pin:
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
