<#
.SYNOPSIS
    Windows/PowerShell equivalent of scripts/bench.sh.

    Builds the image, runs `do ^RunScript` a few times to capture the reported
    elapsed time, then validates the produced CSV independently of row order.

.PARAMETER Runs
    How many timed runs of do ^RunScript to perform (default 5).

.EXAMPLE
    pwsh scripts/bench.ps1 3
#>
[CmdletBinding()]
param(
    [int]$Runs = 5
)

$ErrorActionPreference = 'Stop'

# Resolve the repo root (parent of this script's folder) and work from there.
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# Pick a Python launcher: prefer `python`, fall back to the `py -3` launcher.
if (Get-Command python -ErrorAction SilentlyContinue) {
    $pyExe = 'python'; $pyArgs = @()
} elseif (Get-Command py -ErrorAction SilentlyContinue) {
    $pyExe = 'py'; $pyArgs = @('-3')
} else {
    throw 'No Python interpreter found (looked for `python` and `py`).'
}

docker compose up --build -d

# Wait for IRIS to accept sessions before timing anything. The very first run
# also pays the one-time fluxscan.so compile, so give it a generous window.
Write-Host '== waiting for IRIS to become ready =='
$ready = $false
for ($i = 1; $i -le 60; $i++) {
    $probe = "halt`n" | docker compose exec -T iris iris session iris -U USER 2>$null
    if ($LASTEXITCODE -eq 0 -and $probe -match 'USER>') { $ready = $true; break }
    Start-Sleep -Seconds 2
}
if (-not $ready) { throw 'IRIS did not become ready in time.' }

Write-Host "== timing do ^RunScript over $Runs run(s) =="
# `do ^RunScript` followed by `halt` is fed on stdin.
$driver = "do ^RunScript`nhalt`n"
for ($n = 1; $n -le $Runs; $n++) {
    Write-Host -NoNewline ("run {0}: " -f $n)
    $out = $driver | docker compose exec -T iris iris session iris -U USER
    $line = $out | Select-String -Pattern 'Elapsed|Matched'
    if ($line) { $line.Line.Trim() | ForEach-Object { Write-Host $_ } }
    else { Write-Host '(no timing line captured)' }
}

Write-Host '== validating output =='
& $pyExe @($pyArgs + 'scripts/validate.py')
exit $LASTEXITCODE
