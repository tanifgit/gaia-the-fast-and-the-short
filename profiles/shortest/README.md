# Shortest (code-golf) profile

A separate, switchable submission profile for the **shortest** track. It is not
part of the fastest track and must be measured on its own — do not count the
native C kernel's source when sizing this profile.

## What it does

A single ObjectScript line shells out to Embedded Python. Python pipes every
input file through `zcat` and keeps only data lines with `grep ^[0-9]`, parses
each flux array with `eval` (mapping the literal `NaN` to `0` so it is dropped
by the `x>0` filter), reduces each band to its min/max, computes the larger BP/RP
percentage swing, and writes qualifying rows to `/o/r.csv`.

Valid flux here means strictly positive; `NaN` and non-positive entries are
discarded. This is deliberately less defensive than the fastest track (it does
not guard against `null`/`inf` tokens) because the benchmark inputs only use
`NaN` as an invalid marker, matching the public golf submissions.

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

