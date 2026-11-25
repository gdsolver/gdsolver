# self_test fixtures: a replay that compared nothing must not pass. Run after building tools/self_test.cpp.
#   powershell -File tools/self_test_fixtures.ps1 -Exe <self_test.exe> [-Capture <dir with btraw.csv ctraw.csv objrects.txt>]
param([Parameter(Mandatory = $true)][string]$Exe, [string]$Capture = '')
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('self_test_fx_' + [guid]::NewGuid().ToString('N')); New-Item -ItemType Directory $tmp | Out-Null
$fail = 0
function Check($name, $code, $want) { if ($code -eq $want) { "ok   $name (exit $code)" } else { "FAIL ${name}: exit $code, wanted $want"; $script:fail++ } }
# 1. empty traces: headers only
Set-Content "$tmp\btraw.csv" 'attempt,tick,who,fn,arg,ret,pre,post' -Encoding ascii
Set-Content "$tmp\ctraw.csv" 'attempt,tick,who,fn,arg,ret,pre,post' -Encoding ascii
Set-Content "$tmp\objrects.txt" 'id,type,cx,cy,w,h,groups,uid' -Encoding ascii
& $Exe "$tmp\btraw.csv" "$tmp\objrects.txt" "$tmp\ctraw.csv" | Out-Null; Check 'empty trace' $LASTEXITCODE 3
# 2. a real trace asked for a range it does not have
if ($Capture -and (Test-Path "$Capture\btraw.csv")) {
    & $Exe "$Capture\btraw.csv" "$Capture\objrects.txt" "$Capture\ctraw.csv" --from 900000000 --to 900000100 | Out-Null; Check 'range outside the trace' $LASTEXITCODE 3
    & $Exe "$Capture\btraw.csv" "$Capture\objrects.txt" "$Capture\ctraw.csv" | Out-Null; "info the same capture over its own range exits $LASTEXITCODE (0 identical, 1 differs)"
} else { "skip range fixture: no -Capture given" }
Remove-Item $tmp -Recurse -Force
if ($fail) { exit 1 }