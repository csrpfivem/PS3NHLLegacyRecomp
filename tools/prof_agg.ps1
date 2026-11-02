# Boots via run_nhl.ps1 and, at -At seconds, profiles the -Count busiest host threads
# with the PS3_DEBUG console's `prof`, then AGGREGATES the exclusive samples across
# them by symbol (RVAs resolved against the link map). Usage:
#   prof_agg.ps1 [-Tag t] [-At 26] [-Count 6] [-Exclude <host tids>] [-Env @{}]
param([string]$Tag = "agg", [int]$At = 26, [int]$Count = 6, [int]$Seconds = 70, [hashtable]$Env = @{}, [int]$Top = 40)
$w = Split-Path $PSScriptRoot -Parent
$dbg = "$w\nhl\runs\$Tag.dbgcmd.txt"
$job = Start-Job -ScriptBlock { param($s, $sec, $tag, $envh) & $s -Seconds $sec -Tag $tag -Env $envh } `
       -ArgumentList "$w\tools\run_nhl.ps1", $Seconds, $Tag, $Env
Start-Sleep -Seconds ($At - 2)
$p = Get-Process NHLLegacy -ErrorAction SilentlyContinue | Select-Object -First 1
$errtxt = Get-Content "$w\nhl\runs\$Tag.err.log" -ErrorAction SilentlyContinue
$guestHost = @{}; foreach ($m in ($errtxt | Select-String '\[THREAD (\d+)\] host thread started.*hosttid=(\d+)|MAIN guest thread tid=(\d+)')) {
    if ($m.Matches[0].Groups[2].Value) { $guestHost[$m.Matches[0].Groups[2].Value] = 1 } else { $guestHost[$m.Matches[0].Groups[3].Value] = 1 } }
$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep -Seconds 2; $p.Refresh()
$rows = foreach ($t in $p.Threads) { try { [pscustomobject]@{ Id = $t.Id; Ms = $t.TotalProcessorTime.TotalMilliseconds - $a[$t.Id] } } catch {} }
# SPU host threads are the busy ones that are not guest PPU threads.
$hot = $rows | Where-Object { -not $guestHost.ContainsKey("$($_.Id)") } | Sort-Object Ms -Descending | Select-Object -First $Count
$agg = @{}; $total = 0
foreach ($h in $hot) {
    Remove-Item "$dbg.out" -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText($dbg, "prof $($h.Id) 2`n"); Start-Sleep -Seconds 4
    if (-not (Test-Path "$dbg.out")) { continue }
    $inEx = $false
    foreach ($line in Get-Content "$dbg.out") {
        if ($line -match '--- exclusive') { $inEx = $true; continue }
        if ($line -match '--- inclusive') { $inEx = $false; continue }
        if ($inEx -and $line -match '^\s+([\d.]+)%\s+(.+)$') { $k = $matches[2].Trim(); $agg[$k] = $agg[$k] + [double]$matches[1]; $total += [double]$matches[1] }
    }
}
Wait-Job $job | Out-Null; Receive-Job $job | Out-Null; Remove-Job $job
"profiled $(@($hot).Count) SPU host threads; shares below are % of all their samples"
$agg.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top | ForEach-Object {
    $k = $_.Key
    if ($k -match '^rva=0x([0-9A-F]+)') { $k = (& python -I "$w\tools\maplookup.py" "$w\nhl\build\NHLLegacy.map" "0x$($matches[1])" 2>$null) -replace '^0x[0-9a-f]+: ', '' }
    "{0,6:N1}%  {1}" -f (100 * $_.Value / [Math]::Max(1, $total)), $k
}
