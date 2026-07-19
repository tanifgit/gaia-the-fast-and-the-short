"""Orchestrator for the Gaia DR3 epoch-photometry challenge (fastest track).

IRIS calls ``run()`` through ``%SYS.Python`` during ``do ^RunScript``. This module
does no numeric work of its own on the happy path: it resolves the first twenty
input files, makes sure ``fluxscan.so`` exists (compiling it once if the build
step could not), and hands the file list to the C kernel, which inflates,
parses, filters and writes the CSV. A self-contained pure-Python path exists only
as a correctness safety net when a native toolchain or libdeflate is unavailable.
"""

from __future__ import annotations

import ctypes
import os
from pathlib import Path

_MODULE_DIR = Path(__file__).resolve().parent
_KERNEL_SRC = _MODULE_DIR / "fluxscan.c"
_KERNEL_LIB = _MODULE_DIR / "fluxscan.so"

_DEV_ROOT = Path("/home/irisowner/dev")
_DEFAULT_IN = _DEV_ROOT / "data" / "in"
_DEFAULT_OUT = _DEV_ROOT / "data" / "out" / "challenge_output.csv"

_FILE_LIMIT = 20
_CSV_HEADER = ("source_id", "bp_min_flux", "bp_max_flux",
               "rp_min_flux", "rp_max_flux", "percentage_change")


def _input_files(in_dir: Path) -> list[Path]:
    """First twenty EpochPhotometry gzip files, lexicographically ordered."""
    hits = sorted(in_dir.glob("EpochPhotometry_*.gz"))
    if not hits:
        hits = sorted(in_dir.glob("*.gz"))
    return hits[:_FILE_LIMIT]


def _worker_count() -> int:
    """Leave a little headroom under the visible core count.

    Inflate-plus-scan is bandwidth bound, so a full oversubscription inside a
    container tends to lose to a slightly smaller pool.
    """
    try:
        cores = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        cores = os.cpu_count() or 1
    return max(1, cores - max(1, cores // 4))


def _build_kernel() -> None:
    """Compile fluxscan.c once. Tries a tuned build, falls back to portable.

    libdeflate is loaded by the kernel itself at run time (dlopen), so it is not
    named on the link line here; we only need libdl and libm.
    """
    if _KERNEL_LIB.exists():
        return
    import subprocess

    common = ["-fopenmp", "-fPIC", "-shared", str(_KERNEL_SRC),
              "-ldl", "-lm", "-o", str(_KERNEL_LIB)]
    tuned = ["gcc", "-O3", "-march=native", "-funroll-loops", *common]
    plain = ["gcc", "-O3", *common]
    try:
        subprocess.run(tuned, check=True)
    except Exception:
        subprocess.run(plain, check=True)


def _bind_kernel() -> ctypes.CDLL:
    _build_kernel()
    lib = ctypes.CDLL(str(_KERNEL_LIB))
    lib.flux_scan.restype = ctypes.c_long
    lib.flux_scan.argtypes = [
        ctypes.POINTER(ctypes.c_char_p),  # paths
        ctypes.c_int,                     # count
        ctypes.c_int,                     # threads
        ctypes.c_char_p,                  # output path
    ]
    return lib


def _pure_python(files: list[Path], out_path: Path) -> int:
    """Reference-quality fallback. Correct, not fast; never wins the fastest track."""
    import csv
    import gzip
    import math

    def band(cell: str):
        cell = cell.strip().strip('"')
        if cell.startswith("["):
            cell = cell[1:-1] if cell.endswith("]") else cell[1:]
        lo = hi = None
        for tok in cell.split(","):
            tok = tok.strip()
            if not tok or tok.lower() in ("nan", "null"):
                continue
            try:
                v = float(tok)
            except ValueError:
                continue
            if v > 0.0 and math.isfinite(v):
                lo = v if lo is None or v < lo else lo
                hi = v if hi is None or v > hi else hi
        if lo is None:
            return None
        return lo, hi, (hi - lo) / lo * 100.0

    out_path.parent.mkdir(parents=True, exist_ok=True)
    kept = 0
    with out_path.open("w", newline="") as sink:
        writer = csv.writer(sink)
        writer.writerow(_CSV_HEADER)
        for path in files:
            with gzip.open(path, "rt", newline="") as handle:
                reader = csv.DictReader(row for row in handle if not row.startswith("#"))
                for record in reader:
                    b = band(record["bp_flux"])
                    r = band(record["rp_flux"])
                    pct = max(b[2] if b else -1.0, r[2] if r else -1.0)
                    if pct <= 100.0:
                        continue
                    writer.writerow([
                        record["source_id"],
                        f"{b[0]:.17g}" if b else "", f"{b[1]:.17g}" if b else "",
                        f"{r[0]:.17g}" if r else "", f"{r[1]:.17g}" if r else "",
                        f"{pct:.17g}",
                    ])
                    kept += 1
    return kept


def run(in_dir: str = str(_DEFAULT_IN), out_path: str = str(_DEFAULT_OUT)) -> int:
    """Produce the challenge CSV and return the qualifying-source count."""
    src = Path(in_dir)
    dst = Path(out_path)
    dst.parent.mkdir(parents=True, exist_ok=True)

    files = _input_files(src)
    if not files:
        raise RuntimeError(f"no Gaia .gz inputs under {src}")

    try:
        lib = _bind_kernel()
        encoded = (ctypes.c_char_p * len(files))(*[str(p).encode() for p in files])
        kept = lib.flux_scan(encoded, len(files), _worker_count(), str(dst).encode())
        if kept < 0:
            raise RuntimeError(f"flux_scan returned {kept}")
        return int(kept)
    except Exception as err:  # toolchain/libdeflate missing, etc.
        print(f"native kernel unavailable ({err}); falling back to pure Python")
        return _pure_python(files, dst)
