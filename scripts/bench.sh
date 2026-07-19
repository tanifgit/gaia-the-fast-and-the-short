#!/usr/bin/env bash
# Build the image, run do ^RunScript a few times to capture the reported elapsed
# time, then validate the produced CSV independently of row order.
set -euo pipefail

runs="${1:-5}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$here"

docker compose up --build -d

# Wait for IRIS to accept sessions; the first run also pays the one-time
# fluxscan.so compile, so allow a generous window.
echo "== waiting for IRIS to become ready =="
for _ in $(seq 1 60); do
    if printf 'halt\n' | docker compose exec -T iris iris session iris -U USER 2>/dev/null \
        | grep -q 'USER>'; then
        break
    fi
    sleep 2
done

echo "== timing do ^RunScript over $runs run(s) =="
for n in $(seq 1 "$runs"); do
    printf 'run %s: ' "$n"
    # Feed the routine on stdin.
    printf 'do ^RunScript\nhalt\n' \
        | docker compose exec -T iris iris session iris -U USER \
        | grep -iE 'elapsed|matched' || true
done

echo "== validating output =="
python3 scripts/validate.py
