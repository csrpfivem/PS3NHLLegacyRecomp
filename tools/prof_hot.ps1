# Boots via run_nhl.ps1 and, at -At seconds, samples the -Count busiest host threads
# (by CPU over a 2 s window) with the PS3_DEBUG console's `prof`, then resolves the
# top RVAs against the link map. Usage: prof_hot.ps1 [-Tag t] [-At 30] [-Count 3] [-Env @{}]
param([string]$Tag = "hot", [int]$At = 30, [int]$Count = 3, [int]$Seconds = 70, [hashtable]$Env = @{})
$w = Split-Path $PSScriptRoot -Parent
$dbg = "$w\nhl\runs\$Tag.dbgcmd.txt"
$job = Start-Job -ScriptBlock { param($s, $sec, $tag, $envh) & $s -Seconds $sec -Tag $tag -Env $envh } `
       -ArgumentList "$w\tools\run_nhl.ps1", $Seconds, $Tag, $Env
Start-Sleep -Seconds ($At - 2)
$p = Get-Process NHLLegacy -ErrorAction SilentlyContinue | Select-Object -First 1
$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep -Seconds 2; $p.Refresh()
$rows = foreach ($t in $p.Threads) { try { [pscustomobject]@{ Id = $t.Id; Ms = $t.TotalProcessorTime.TotalMilliseconds - $a[$t.Id] } } catch {} }
$hot = $rows | Sort-Object Ms -Descending | Select-Object -First $Count
$out = @()
foreach ($h in $hot) {
    Remove-Item "$dbg.out" -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText($dbg, "prof $($h.Id) 3`n"); Start-Sleep -Seconds 5
    if (Test-Path "$dbg.out") { $out += "=== host tid $($h.Id) ($([int]($h.Ms/20))% CPU)"; $out += Get-Content "$dbg.out" }
}
Wait-Job $job | Out-Null; Receive-Job $job | Out-Null; Remove-Job $job
$errtxt = Get-Content "$w\nhl\runs\$Tag.err.log" -ErrorAction SilentlyContinue
foreach ($line in $out) {
    if ($line -match '^\s+([\d.]+)%\s+rva=0x([0-9A-F]+)') {
        $sym = & python -I "$w\tools\maplookup.py" "$w\nhl\build\NHLLegacy.map" "0x$($matches[2])" 2>$null
        "{0,7}%  {1}" -f $matches[1], ($sym -replace '^0x[0-9a-f]+: ', '')
    } elseif ($line -match '===|exclusive|inclusive|samples') { $line }
    elseif ($line -match '^\s+([\d.]+)%\s+(\S+)') { "{0,7}%  {1}" -f $matches[1], $matches[2] }
}
$h2g = @{}; foreach ($m in ($errtxt | Select-String '\[THREAD (\d+)\] host thread started.*hosttid=(\d+)')) { $h2g[$m.Matches[0].Groups[2].Value] = $m.Matches[0].Groups[1].Value }
foreach ($h in $hot) { "host $($h.Id) = guest tid $(if ($h2g.ContainsKey("$($h.Id)")) { $h2g["$($h.Id)"] } else { '? (SPU or runtime)' })" }
