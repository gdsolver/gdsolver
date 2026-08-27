#Requires -Version 5.1
<#
.SYNOPSIS
  Prove that two builds of the solver behave identically.

.DESCRIPTION
  Runs both executables over the same suite and compares the produced plan,
  trace and band files byte for byte. This is the acceptance instrument for
  behaviour-preserving changes to dp/ (see rule 3 in CLAUDE.md): a
  refactoring that is really a refactoring produces identical bytes.

  Two kinds of case:
    replay  re-simulate a known solution (exercises the physics, not the search)
    cold    solve from tick 0 with a bounded horizon (exercises the search too)

  stdout is compared as well, ignoring the lines that name the executable or a
  wall-clock duration.

.EXAMPLE
  # after rebuilding, against the frozen reference
  tools\equiv_check.ps1 -OldExe ..\GD-lab\exe\leveldp_r108b.exe `
                        -NewExe build\dp\RelWithDebInfo\leveldp.exe
#>
[CmdletBinding()]
param(
    # The reference build. Keep frozen copies of it; never compare against a
    # binary that may be rebuilt under you.
    [string]$OldExe,
    [string]$NewExe = 'build\dp\RelWithDebInfo\leveldp.exe',
    [string]$OutDir = "$env:TEMP\gdsolver-equiv",
    # Solutions live in the repository; the level dumps live in the private lab.
    [string]$Data = 'data',
    [string]$LevelData = $(if ($env:GDSOLVER_LAB) { "$env:GDSOLVER_LAB\data" } else { '..\GD-lab\data' }),
    [int[]]$ReplayLevels = @(1, 16, 18, 20, 22),
    [int[]]$ColdLevels = @(1..22),
    [int]$ColdHorizon = 3000
)
$ErrorActionPreference = 'Stop'
if (-not $OldExe) { throw 'Pass -OldExe: the frozen build to compare against.' }
foreach ($e in @($OldExe, $NewExe)) { if (-not (Test-Path $e)) { throw "not found: $e" } }
New-Item -ItemType Directory -Force $OutDir | Out-Null

function LevelArgs([int]$lv) {
    $a = @()
    $trig = "$LevelData\triggers_lv$lv.txt"
    $grp = "$LevelData\objgroups_lv$lv.txt"
    $obb = "$LevelData\obb_lv$lv.txt"
    if ((Test-Path $trig) -and (Test-Path $grp)) { $a += @('--triggers', $trig, '--objgroups', $grp) }
    if (Test-Path $obb) { $a += @('--obb', $obb) }
    return $a
}
function GroupsArgs([string]$plan) {
    # The overriding order matters and must match the driver's: base, then the
    # deep (nodeath) recording, then the live one, then the ride bank.
    $a = @()
    foreach ($suf in '.groups.txt', '.groups.deep.txt', '.groups.live.txt', '.groups.bank.txt') {
        $p = "$plan$suf"
        if ((Test-Path $p) -and ((Get-Item $p).Length -gt 0)) { $a += @('--groups', $p) }
    }
    return $a
}
function RunCase([string]$name, [string[]]$argsTemplate) {
    $res = @{}
    foreach ($side in 'old', 'new') {
        $exe = if ($side -eq 'old') { $OldExe } else { $NewExe }
        $out = "$OutDir\$name.$side"
        $args = $argsTemplate | ForEach-Object { $_ -replace '@OUT@', $out }
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $stdout = & $exe @args 2>&1 | Out-String
        $sw.Stop()
        Set-Content -Path "$out.stdout.txt" -Value $stdout -Encoding utf8
        $res[$side] = @{ out = $out; secs = [math]::Round($sw.Elapsed.TotalSeconds, 1) }
    }
    $verdict = @()
    $ok = $true
    foreach ($suf in '', '.trace.csv', '.bands.txt') {
        $o = $res.old.out + $suf; $n = $res.new.out + $suf
        if (-not (Test-Path $o) -and -not (Test-Path $n)) { continue }
        if (-not (Test-Path $o) -or -not (Test-Path $n)) { $verdict += "MISSING$suf"; $ok = $false; continue }
        if ((Get-FileHash $o -Algorithm MD5).Hash -eq (Get-FileHash $n -Algorithm MD5).Hash) {
            $verdict += "SAME$suf"
        } else { $verdict += "DIFF$suf"; $ok = $false }
    }
    # Each side writes to its own files and is its own executable, so the paths
    # and timings in stdout differ by construction: normalise those away and
    # compare what is left, which is the run's actual report.
    $skip = '\bsec\b|\bms\b|elapsed|wall'
    $norm = {
        param($line, $base)
        ($line -replace [regex]::Escape($base), '<OUT>') -replace '(?i)[^\s]*leveldp[^\s]*\.exe', '<EXE>'
    }
    $so = Get-Content "$($res.old.out).stdout.txt" |
          Where-Object { $_ -notmatch $skip } |
          ForEach-Object { & $norm $_ $res.old.out }
    $sn = Get-Content "$($res.new.out).stdout.txt" |
          Where-Object { $_ -notmatch $skip } |
          ForEach-Object { & $norm $_ $res.new.out }
    $d = @(Compare-Object -ReferenceObject @($so) -DifferenceObject @($sn))
    if ($d.Count) { $ok = $false }
    $verdict += "stdout_diff=$($d.Count)"
    Write-Host ("{0,-14} old {1,6}s new {2,6}s  {3}" -f $name, $res.old.secs,
                $res.new.secs, ($verdict -join ' '))
    return $ok
}

$fail = 0
foreach ($lv in $ReplayLevels) {
    $plan = "$Data\solution_lv${lv}_dp.txt"
    if (-not (Test-Path $plan)) { "replay lv${lv}: no solution, skipped"; continue }
    $a = @("$LevelData\objrects_lv$lv.txt", '--replay', $plan, '--out', '@OUT@') +
         (LevelArgs $lv) + (GroupsArgs $plan)
    if (-not (RunCase "replay_lv$lv" $a)) { $fail++ }
}
foreach ($lv in $ColdLevels) {
    $obj = "$LevelData\objrects_lv$lv.txt"
    if (-not (Test-Path $obj)) { "cold lv${lv}: no level dump, skipped"; continue }
    $a = @($obj, '--out', '@OUT@', '--cap', '2000', '--shipyq', '0.25', '--shipvq', '1',
           '--threads', '8') + (LevelArgs $lv) +
         @('--needtrig-unseen', '--bands', '@OUT@.bands.txt', '--horizon', "$ColdHorizon")
    if (-not (RunCase "cold_lv$lv" $a)) { $fail++ }
}
if ($fail) { Write-Host "$fail case(s) differ" -ForegroundColor Red; exit 1 }
Write-Host 'identical' -ForegroundColor Green
