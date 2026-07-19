#!/usr/bin/env python3
"""Order-independent validator for the Gaia DR3 challenge output.

Builds an independent pure-Python reference from ``data/in/*.gz`` and compares it,
keyed by ``source_id``, against ``data/out/challenge_output.csv``. Row order is
irrelevant; small floating-point formatting differences are tolerated. The script
exits non-zero if any qualifying row is missing, any unexpected row is present, or
any numeric cell differs beyond tolerance.
"""

from __future__ import annotations

import csv
import glob
import gzip
import math
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IN_DIR = REPO / "data" / "in"
OUT_CSV = REPO / "data" / "out" / "challenge_output.csv"

FILE_LIMIT = 20
CUTOFF = 100.0
REL_TOL = 1e-10
ABS_TOL = 1e-8
EXPECTED_COLUMNS = [
    "source_id", "bp_min_flux", "bp_max_flux",
    "rp_min_flux", "rp_max_flux", "percentage_change",
]


def _band(cell: str):
    """Return (min, max, pct) over strictly-positive finite flux, or None."""
    cell = cell.strip().strip('"')
    if cell.startswith("["):
        cell = cell[1:-1] if cell.endswith("]") else cell[1:]
    lo = hi = None
    for token in cell.split(","):
        token = token.strip()
        if not token or token.lower() in ("nan", "null"):
            continue
        try:
            value = float(token)
        except ValueError:
            continue
        if value > 0.0 and math.isfinite(value):
            lo = value if lo is None or value < lo else lo
            hi = value if hi is None or value > hi else hi
    if lo is None:
        return None
    return lo, hi, (hi - lo) / lo * 100.0


def build_reference():
    """Map source_id -> (bp_min, bp_max, rp_min, rp_max, pct) for qualifying rows."""
    files = sorted(IN_DIR.glob("EpochPhotometry_*.gz"))
    if not files:
        files = sorted(Path(p) for p in glob.glob(str(IN_DIR / "*.gz")))
    files = files[:FILE_LIMIT]
    if not files:
        raise SystemExit(f"no input files under {IN_DIR}")

    reference = {}
    total = 0
    for path in files:
        with gzip.open(path, "rt", newline="") as handle:
            reader = csv.DictReader(row for row in handle if not row.startswith("#"))
            for record in reader:
                total += 1
                bp = _band(record["bp_flux"])
                rp = _band(record["rp_flux"])
                pct = max(bp[2] if bp else -1.0, rp[2] if rp else -1.0)
                if pct <= CUTOFF:
                    continue
                reference[record["source_id"]] = (
                    bp[0] if bp else None, bp[1] if bp else None,
                    rp[0] if rp else None, rp[1] if rp else None,
                    pct,
                )
    return reference, total, len(files)


def load_output():
    if not OUT_CSV.exists():
        raise SystemExit(f"output not found: {OUT_CSV}")
    with OUT_CSV.open(newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header != EXPECTED_COLUMNS:
            raise SystemExit(f"header mismatch:\n got: {header}\n want: {EXPECTED_COLUMNS}")
        rows = {}
        for line in reader:
            if len(line) != 6:
                raise SystemExit(f"row without six columns: {line}")
            rows[line[0]] = line
    return rows


def _num(text: str):
    text = text.strip()
    return None if text == "" else float(text)


def _close(a, b) -> bool:
    if a is None and b is None:
        return True
    if a is None or b is None:
        return False
    return math.isclose(a, b, rel_tol=REL_TOL, abs_tol=ABS_TOL)


def main() -> int:
    reference, total, nfiles = build_reference()
    produced = load_output()

    missing = reference.keys() - produced.keys()
    extra = produced.keys() - reference.keys()
    mismatched = []

    for sid in reference.keys() & produced.keys():
        want = reference[sid]
        got = produced[sid]
        got_nums = (_num(got[1]), _num(got[2]), _num(got[3]), _num(got[4]), _num(got[5]))
        if not all(_close(w, g) for w, g in zip(want, got_nums)):
            mismatched.append((sid, want, got_nums))

    print(f"files scanned      : {nfiles}")
    print(f"total sources      : {total}")
    print(f"reference qualifying: {len(reference)}")
    print(f"output rows        : {len(produced)}")
    print(f"missing rows       : {len(missing)}")
    print(f"unexpected rows    : {len(extra)}")
    print(f"value mismatches   : {len(mismatched)}")

    ok = not missing and not extra and not mismatched
    if not ok:
        for sid in list(missing)[:5]:
            print(f"  MISSING {sid}")
        for sid in list(extra)[:5]:
            print(f"  EXTRA   {sid}")
        for sid, want, got in mismatched[:5]:
            print(f"  DIFF    {sid}: want={want} got={got}")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
