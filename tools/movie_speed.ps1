# Boots via run_nhl.ps1 and measures movie playback speed in wall-clock time: the VP6
# bridge logs its 1st and 301st published frame, so (t301 - t1) should be 10.0 s for a
# 29.97 fps movie played at full speed. Also reports presents (title-bar FPS) over the
# same window from the [fps]-free flip count is not available live, so it reports the
# ratio only. Usage: movie_speed.ps1 [-Tag name] [-Seconds 90] [-Env @{...}]
param([string]$Tag = "mspd", [int]$Seconds = 90, [hashtable]$Env = @{})

$w = Split-Path $PSScriptRoot -Parent
$err = "$w\nhl\runs\$Tag.err.log"
Remove-Item $err -ErrorAction SilentlyContinue
$job = Start-Job -ScriptBlock { param($s, $sec, $tag, $envh) & $s -Seconds $sec -Tag $tag -Env $envh } `
       -ArgumentList "$w\tools\run_nhl.ps1", $Seconds, $Tag, $Env
$t0 = Get-Date; $seen = @{}
while ($job.State -eq 'Running') {
    Start-Sleep -Milliseconds 200
    if (-not (Test-Path $err)) { continue }
    $lines = Get-Content $err -ErrorAction SilentlyContinue | Select-String -Pattern '\[vp6bridge\] desc=0x[0-9A-F]+ (start (\S+)|frame (\d+) )'
    foreach ($m in $lines) {
        $k = $m.Line
        if (-not $seen.ContainsKey($k)) { $seen[$k] = [Math]::Round(((Get-Date) - $t0).TotalSeconds, 2) }
    }
}
Receive-Job $job | Where-Object { $_ -match 'flips' }; Remove-Job $job
$seen.GetEnumerator() | Sort-Object Value | ForEach-Object { "{0,7:N2} s  {1}" -f $_.Value, ($_.Key -replace '^\[vp6bridge\] ', '') }
$f1 = $seen.GetEnumerator() | Where-Object { $_.Key -match 'frame 1 \(' } | Sort-Object Value | Select-Object -First 1
$f301 = $seen.GetEnumerator() | Where-Object { $_.Key -match 'frame 301 \(' } | Sort-Object Value | Select-Object -First 1
if ($f1 -and $f301) {
    $dt = $f301.Value - $f1.Value
    "300 movie frames took {0:N1} s -> {1:N1} movie fps ({2:P0} of full speed at 29.97)" -f $dt, (300 / $dt), ((300 / $dt) / 29.97)
}
