# Gaia DR3 epoch-photometry challenge - scanning data (fast... or short...)

An entry for InterSystems Employee Programming Challenge (#1). 
It scans the first twenty Gaia DR3 epoch-photometry archive files,
finds sources whose BP or RP flux swings by more than 100 %, 
and writes them to a CSV. 

Two tracks are provided:

- **Fastest** (default) - IRIS orchestrates, Embedded Python loads a native
  C/libdeflate/OpenMP kernel that does the entire calculation and writes the CSV.
- **Shortest** (code-golf) - a single-line ObjectScript (uses Python) entry in
  [profiles/shortest/](profiles/shortest/).

## Pre-Requisites and Getting Started
You’ll need Git installed as well as Docker Desktop

Then you can run this to clone the repo and change into the new folder:

```bash
git clone https://github.com/tanifgit/gaia-the-fast-and-the-short.git
cd gaia-the-fast-and-the-short
```


## Quick start

Interactively:

```bash
docker compose up --build -d
docker compose exec iris iris session iris
USER>do ^RunScript
```

Or scripted, in one line (same stdin idiom used everywhere else in this README):

```bash
docker compose up --build -d
printf 'do ^RunScript\nhalt\n' | docker compose exec -T iris iris session iris -U USER
```

`do ^RunScript` prints the number of matched sources and the elapsed time, and
writes the result to:

```
data/out/challenge_output.csv
```

## Output

The CSV header is exactly:

```
source_id,bp_min_flux,bp_max_flux,rp_min_flux,rp_max_flux,percentage_change
```

For every `source_id` the kernel parses the `bp_flux` and `rp_flux` arrays,
computes each band's min and max over valid flux, and derives:

```
percentage_change = ((max_flux - min_flux) / min_flux) * 100
```

`percentage_change` is the larger of the BP and RP values, and only rows above
100 are emitted.

### Valid flux handling

A value counts only if it is a **strictly positive finite** number. Missing,
null, `NaN`, zero, negative, and non-finite entries are ignored. A band with no
valid value contributes nothing and leaves its columns empty.

### Row ordering

Output order is **deterministic but not sorted by percentage**. Rows appear in
input-file order (files in lexicographic name order, records in file order). The
contest does not specify or require any ordering, so the default track skips a
sort entirely to save time; ordering is semantically irrelevant. Use
[scripts/validate.py](scripts/validate.py) to compare result sets independent of
row order.

## Fastest design

```
do ^RunScript (RunScript.mac)
   ├─ [pre-timing]  flux_runner.extract()   inflate data/in/*.gz -> /tmp/gaia_tmp/*.csv
   │                                          (the template's "extract files ... to
   │                                           data/temp" step, above Set startTime)
   └─ [timed]       %SYS.Python ─▶ flux_runner.run() ─▶ fluxscan.so (C)
                                               ├─ mmap each plain .csv (zero-copy)
                                               ├─ single in-place CSV walk, cols 1/11/16
                                               ├─ per-band min/max + percentage
                                               └─ format qualifying rows into a per-file buffer
```

- **libdeflate**, not Python `gzip`, does decompression.
- **OpenMP** runs one task per file; the biggest compressed streams are
  scheduled first so the longest inflate jobs start at time zero.
- Each file thread owns a private growable byte buffer. After all threads finish,
  the driver concatenates the buffers **in the caller's file order**, so output is
  deterministic with no global lock and no post-sort.
- The parser reads only the three needed columns and never materialises per-value
  arrays. Qualifying CSV lines are formatted directly in C.
- A compact custom positive-float parser handles decimal and scientific notation
  and rejects `NaN`, blanks, null, zero, negative and non-finite tokens.
- The default path never imports Polars, pandas or NumPy, and does no per-row
  work in Python.

### Performance tuning

Profiling showed that gzip decompression accounts for roughly **99% of the runtime**. 
Scanning the resulting ~1.5 GB of CSV data takes very little time, 
so parser and buffer optimizations would have little practical impact.

The three changes that made a measurable difference were:

- **Decompress before timing starts.** The benchmark template treats extraction as setup, so `extract()` decompresses the archives into `/tmp/gaia_tmp` before `startTime` is set. The timed `run()` phase only parses the CSV files.

- **Keep extraction and scanning on the container filesystem.** Docker bind mounts were much slower than container-local storage, even with a warm cache. The compressed files are copied to `/tmp/gaia_in` during the image build, extracted to `/tmp/gaia_tmp`, and scanned from there.

- **Scan the CSV files in place.** `run()` memory-maps each decompressed file and parses it directly, avoiding additional allocations and data copies.

Several other approaches were benchmarked but did not produce a reliable improvement: newer or locally built libdeflate versions, Intel ISA-L, `-Ofast`, LTO, PGO, static linking, huge pages, buffer and decompressor reuse, `MAP_POPULATE`, `MAP_SHARED`, and different `read()`/`mmap`/`madvise` combinations.

Cache pre-warming and reducing Python startup overhead only improved the first run. But since the benchmark makes a few runs, later runs already benefit from a warm filesystem cache and loaded Python modules.


A pure-Python fallback lives in `flux_runner.py` for environments without a C
toolchain or libdeflate. It is correct but intentionally not competitive; it
exists only so the submission still produces the right CSV everywhere.

### A note on Embedded Python and `%SYS.Python`

`%SYS.Python` is excellent as an *orchestration* seam: IRIS stays in control and
hands one call to native code. It is the wrong place for hot loops - parsing
every flux value in Python would dominate the runtime. This design therefore
keeps Python to file discovery and a single kernel call, and pushes all per-row
work into C.

## Validation

```bash
python3 scripts/validate.py
```

The validator rebuilds an independent reference straight from `data/in/*.gz`,
keys both sides by `source_id`, and compares flux and percentage cells with
relative tolerance `1e-10` / absolute tolerance `1e-8`. It fails on any missing
row, any unexpected row, or any material numeric difference. For the standard
20-file set it reports roughly **75,068 total sources** and **57,099 qualifying
sources**.

## Benchmark

```bash
bash scripts/bench.sh 3      # build, time do ^RunScript 3×, then validate
```

On Windows, use the PowerShell equivalent (no bash required):

```powershell
pwsh scripts/bench.ps1 3     # or: powershell -File scripts/bench.ps1 3
```

Both scripts build the image, wait for IRIS, time `do ^RunScript`, and run the
validator; `bench.ps1` uses `python` (falling back to the `py -3` launcher).

## Shortest profile

See [profiles/shortest/README.md](profiles/shortest/README.md). In short:

```bash
# Switch entrypoints non-destructively: back up the default first.
cp src/RunScript.mac src/RunScript.mac.fastest
cp profiles/shortest/RunScript.mac src/RunScript.mac
docker compose up --build -d
printf 'do ^RunScript\nhalt\n' | docker compose exec -T iris iris session iris -U USER

# Restore the default fastest entrypoint when done.
cp src/RunScript.mac.fastest src/RunScript.mac
```

The default `src/RunScript.mac` is the fastest-track entrypoint; the copy above
overwrites it, so back it up (as shown) before switching and restore it
afterwards. `.mac.fastest` is not committed. It writes `data/out/r.csv` via the
`/i` and `/o` compose aliases.

## Repository layout

```
README.md                    this file
Dockerfile                   stages inputs to /tmp/gaia_in, prebuilds fluxscan.so
docker-compose.yml           template mount + /i and /o golf aliases
iris.script                  compiles src/ and enables Embedded Python call-in
merge.cpf                    IRIS CPF merge (from template)
src/RunScript.mac            entrypoint: extract() pre-timing, then timed run()
src/flux_runner.py           orchestrator: extract() + run() + pure-Python fallback
src/fluxscan.c               native scan kernel (plain-CSV zero-copy + gzip paths)
profiles/shortest/           code-golf profile + its README
scripts/validate.py          order-independent result validator
scripts/bench.sh             build + time + validate helper
data/in/                     the 20 benchmark .gz files (from template)
data/out/                    output directory (CSV not committed)
```

## Licence

See [LICENSE](LICENSE).
