# Boots via run_nhl.ps1 and timestamps progress markers as they appear in the run's
# stderr log -- the wall-clock time to reach each boot stage, which is the speed the
# user actually feels (the title-bar FPS counts presents, not game updates).
# Usage: measure_boot.ps1 -Tag name [-Seconds 200] [-Env @{...}]
param([string]$Tag = "meas", [int]$Seconds = 200, [hashtable]$Env = @{})

$w = Split-Path $PSScriptRoot -Parent
$err = "$w\nhl\runs\$Tag.err.log"
Remove-Item $err -ErrorAction SilentlyContinue
$markers = [ordered]@{
    'EA logo movie' = 'EAlogo\.vp6';
    'title screen'  = '03_pressstart\.big';
    'title bg'      = 'nhltitlebackground\.big' }
$job = Start-Job -ScriptBlock { param($s, $sec, $tag, $envh) & $s -Seconds $sec -Tag $tag -Env $envh } `
       -ArgumentList "$w\tools\run_nhl.ps1", $Seconds, $Tag, $Env
$t0 = Get-Date; $hit = @{}
while ($job.State -eq 'Running') {
    Start-Sleep -Milliseconds 500
    if (Test-Path $err) {
        $txt = Get-Content $err -Raw -ErrorAction SilentlyContinue
        foreach ($k in $markers.Keys) {
            if (-not $hit.ContainsKey($k) -and $txt -and $txt -match $markers[$k]) {
                $hit[$k] = [Math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
        }
    }
}
$out = Receive-Job $job; Remove-Job $job
$fl = ($out | Where-Object { $_ -match 'flips' }) -join ''
foreach ($k in $markers.Keys) { "{0,-14} {1}" -f $k, $(if ($hit.ContainsKey($k)) { "$($hit[$k]) s" } else { "not reached" }) }
"flips: $fl"
