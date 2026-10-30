"""Verify the recorded golden corpus without compiling DSP dependencies."""

import hashlib
import json
from pathlib import Path
import sys


def check_golden(root):
    provenance = json.loads((root / "provenance.json").read_text(encoding="utf-8"))
    expected = set()
    for record in provenance["files"]:
        path = (root / record["path"]).resolve()
        if not path.is_relative_to(root.resolve()) or record["path"] in expected:
            raise ValueError("Invalid or duplicate golden path")
        expected.add(record["path"])
        data = path.read_bytes()
        if len(data) != record["size"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
            raise ValueError(f"Golden file differs: {record['path']}")
    actual = {p.relative_to(root).as_posix() for group in ("core", "vocal", "netsim", "net")
              for p in (root / group).rglob("*") if p.is_file()}
    if not expected or actual != expected:
        raise ValueError("Golden file inventory differs")
    return len(expected)


if __name__ == "__main__":
    try:
        root = Path(__file__).resolve().parent / "golden"
        print(f"Golden corpus integrity: {check_golden(root)} files match recorded SHA-256")
    except (ValueError, KeyError, OSError) as error:
        print(f"Golden check failed: {error}", file=sys.stderr)
        sys.exit(1)
