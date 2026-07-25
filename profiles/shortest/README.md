# Shortest (code-golf) profile

A separate, switchable submission profile for the **shortest** track. It is not
part of the fastest track and must be measured on its own - do not count the
native C kernel's source when sizing this profile.
This leave RunScript.mac at a bare minimum.


## What it does

A single ObjectScript line shells out to Embedded Python. Python pipes every
input file through `zcat` and keeps only data lines with `grep ^[0-9]`, then for
each row:

- Parses the two flux arrays (columns 11 and 16, selected with the slice
  `s[11:17:5]`) with `eval`, mapping the literal `NaN` to `0`.
- Reduces each band to its valid values with `filter(None, …)` - which drops the
  `0`s (the mapped `NaN`s) - and takes `min`/`max` of what remains.
- Computes each band's ratio `(max-min)/min`, keeps the larger, and emits the row
  when that ratio exceeds `1` (i.e. percentage change > 100), writing the
  percentage as `ratio*100`.

Output columns are `source_id, bp_min_flux, bp_max_flux, rp_min_flux,
rp_max_flux, percentage_change`, in that order, written to `/o/r.csv`.

### Additional golf choices (correctness caveats)

- **No header row.** The contest asks for a CSV *with those columns*; it does not
  require a column-name header line, so the shortest profile omits it to save
  characters. (The fastest track still writes the header.)
- **`filter(None, …)` instead of `x > 0`.** This keeps every non-zero value. It is
  exact for the benchmark data, which uses `NaN` (mapped to `0`) as the only
  invalid marker and contains no negative or zero flux. It is deliberately less defensive than the fastest track, in
  the spirit of code golf.

Both bands are still handled even when one is entirely `NaN` (an empty band
becomes `[0]`, whose `min`/`max` are `0` and is skipped by the `if l` guard in the
ratio step).

## Paths and aliases

- Reads from `/i` (alias of `data/in`, mounted read-only in `docker-compose.yml`).
- Writes `/o/r.csv`, which maps to `data/out/r.csv`.

Both aliases come from the compose file's extra volume entries; the primary
`/home/irisowner/dev` mount is untouched.

## How to submit / run this profile

```bash
cp src/RunScript.mac src/RunScript.mac.fastest        # back up the default first
cp profiles/shortest/RunScript.mac src/RunScript.mac  # switch the entrypoint
docker compose up --build -d
printf 'do ^RunScript\nhalt\n' | docker compose exec -T iris iris session iris -U USER
head -2 data/out/r.csv
```

Restore the fastest track afterwards by copying the backup back:

```bash
cp src/RunScript.mac.fastest src/RunScript.mac
```

