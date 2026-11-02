# Boots the native NHL build with the sandboxed VFS, captures stdout/stderr, and kills it after -Seconds.
# Just before the kill it asks the PS3_DEBUG console for `stat` and `waits` and saves the answers
# to runs\<Tag>.dbg.txt, so every run ends with a diagnosis of where each guest thread is parked.
# Usage: run_nhl.ps1 [-Seconds 60] [-Tag name] [-Env @{ VAR = 'value' }]
# -ProfAt <s>: at that uptime, sample the host threads of guest tids -ProfTids (default main=1,
# render=7) with the console's `prof` command -- where a thread is spinning, by function.
# -DbgExtra: more console commands to run before the kill, after stat/waits (e.g. "mem 2248554 4").
param([int]$Seconds = 60, [string]$Tag = "boot", [hashtable]$Env = @{}, [int]$ProfAt = 0, [int[]]$ProfTids = @(1, 7),
      [string[]]$DbgExtra = @(), [int]$MonitorSec = 0, [switch]$MonitorProf, [int[]]$ProfSpu = @(),
      [string]$Exe = "")
# -Exe <path>: run another build's exe (e.g. nhl\build_lto\NHLLegacy.exe). It still runs from nhl\build,
# so saves (build\gamedata) and the shader cache (build\shadercache) are shared.

$w = Split-Path $PSScriptRoot -Parent
$exe = if ($Exe) { (Resolve-Path $Exe).Path } else { "$w\nhl\build\NHLLegacy.exe" }
$logs = "$w\nhl\runs"
New-Item -ItemType Directory -Force $logs | Out-Null

$dbg = "$logs\$Tag.dbgcmd.txt"
Set-Content $dbg "" -Encoding ascii
Remove-Item "$dbg.out" -ErrorAction SilentlyContinue

$env:PS3_VFS_ROOT  = ("$w\nhl\vfs") -replace '\\', '/'
$env:PS3_HDD0_ROOT = ("$w\nhl\hdd0") -replace '\\', '/'
# PS3_DEV_FLASH is optional (the game does not need dev_flash); set NHL_DEV_FLASH to pass one through.
if ($env:NHL_DEV_FLASH) { $env:PS3_DEV_FLASH = $env:NHL_DEV_FLASH }
$env:PS3_TITLE     = "NHL Legacy (native)"
# Decrypted EDAT/SDATA caches go into the sandbox, never beside the source files
# (the disc tree is a junction to the user's own dump).
$edatCache = "$w\nhl\edat_cache"
New-Item -ItemType Directory -Force $edatCache | Out-Null
$env:PS3_EDAT_CACHE_DIR = $edatCache -replace '\\', '/'
# Defaults every NHL boot needs; -Env overrides any of them.
#   PS3_VERBOSE=0: no per-wait log lines (they starve guest threads on the stderr lock).
#   RD_SPU_INTERP*/SPU_INTERP_UNLIFTED: run the Job Manager SPU kernels interpreted, async.
#   GCM_SEMA_ACQUIRE=1: NV406E ACQUIRE blocks like hardware (EA syncs SPU-built command buffers on it).
#   GCM_NO_AUTOMAP=1: AddressToOffset fails for unmapped EAs, as retail libgcm does.
#   GCM_TITLE_RECYCLE=0: EA recycles its own ring; the runtime's recycle dropped a fence (UI freeze).
#   SPU_MICROSTEP=0: unlifted SPU jobs go straight to the interpreter instead of the
#   branch-at-a-time micro-interpreter (LOCAL_PATCHES #41).
#   SPU_RESIDENT_REJOIN=1: the SPU interpreter hands off to the compiled programs and
#   jobs (LOCAL_PATCHES #44). Without it only the kernel runs compiled -- the boot is
#   ~2x slower. Pass -Env @{ SPU_RESIDENT_REJOIN = '0' } to compare.
#   SPU_POLL_LSA=3EE00: the Job Manager kernel's idle queue poll lands there; an
#   SPU polling an unchanged queue sleeps 100 us between polls instead of spinning
#   (frees the cores the decoding SPU and the PPU threads share). '' = off.
#   GCM_REF_IMMEDIATE=1: publish each SET_REFERENCE as the walker passes it (EA's ring
#   allocator reads ref as GPU position; the one-per-tick queue capped the 3D intro at
#   ~1 fps). LOCAL_PATCHES #61.
#   GCM_PARK_GRACE_MS=10000: EA patches a parked self-JUMP after moving put; the SDK's
#   50 ms grace resynced mid-transition and froze the game (LOCAL_PATCHES #64).
#   NHL_FFMPEG/NHL_MOVIE_DIR: the VP6 host-decode bridge (nhl_vp6_bridge.cpp); movies are
#   carved out of the .big archives by tools\vp6carve.py into nhl\movies.
#   VP_NOFIXPROJ=1: disable the D3D12 backend's vkcube "garbage projection" fallback,
#   which replaced c0-c3 whenever c0.x was outside (0, 8). NHL keeps its VIEW matrix in
#   c0-c3 (c0.x ~ -0.86 for the broadcast camera), so the net/props/players were drawn
#   with a fake camera at the origin -- the in-match "doors" (LOCAL_PATCHES #106).
$ffmpeg = Get-ChildItem "$w\toolchain\ffmpeg" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$defaults = @{ PS3_VERBOSE = '0'; RD_SPU_INTERP = '1'; RD_SPU_INTERP_ASYNC = '1'; SPU_INTERP_UNLIFTED = '1';
               GCM_SEMA_ACQUIRE = '1'; GCM_NO_AUTOMAP = '1'; GCM_TITLE_RECYCLE = '0'; SPU_MICROSTEP = '0';
               SPU_RESIDENT_REJOIN = '1'; SPU_POLL_LSA = '3EE00'; GCM_REF_IMMEDIATE = '1'; GCM_PARK_GRACE_MS = '10000';
               PS3_TICK_LR = '0077363C'; PS3_TICK_TID = '1';   # #140: [HZ] "game N" = main-loop iterations/s
               NHL_MOVIE_DIR = "$w\nhl\movies"; VP_NOFIXPROJ = '1';
               # #154 (play214: dips 40 -> 12 s vs play213 at a similar load): SPU host threads
               # above normal priority, one physical core each. Pass NHL_SPU_PRIO='' /
               # NHL_SPU_CORES='0' to turn off; re-test on 4-core machines (Steam Deck).
               # #258: 'auto' = on only for >= 8 cores with SMT ('1' forces it).
               NHL_SPU_PRIO = 'auto'; NHL_SPU_CORES = 'auto';
               # #163/#164/#168 (play223, user: "super smooth"): start in borderless fullscreen
               # with VRR presents (FreeSync / G-SYNC). F11 toggles; pass NHL_FULLSCREEN='0' for a
               # window (automated smoke tests) and NHL_VRR='0' on a fixed-refresh screen.
               NHL_FULLSCREEN = '1'; NHL_VRR = '1';
               # #221: the exe now has these defaults built in (nhl_config.cpp); this script
               # sets them itself, so '' (= unset in PowerShell) still means off.
               NHL_NO_DEFAULTS = '1';
               PS3_DEBUG = ($dbg -replace '\\', '/') }
if ($ffmpeg) { $defaults['NHL_FFMPEG'] = $ffmpeg.FullName }
foreach ($k in $defaults.Keys) { if (-not $Env.ContainsKey($k)) { $Env[$k] = $defaults[$k] } }
foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }

$out = "$logs\$Tag.out.log"; $err = "$logs\$Tag.err.log"
$p = Start-Process -FilePath $exe -ArgumentList "`"$w\bin\EBOOT_v101.elf`"" -WorkingDirectory "$w\nhl\build" `
     -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
$waitMs = [Math]::Max(1, $Seconds - 5) * 1000
if ($ProfAt -gt 0 -and $ProfAt -lt ($Seconds - 15)) {
    if (-not $p.WaitForExit($ProfAt * 1000)) {
        $errText = Get-Content $err -ErrorAction SilentlyContinue
        foreach ($gt in $ProfTids) {
            $hostTid = $null
            if ($gt -eq 1) { $m = $errText | Select-String '\[boot\] MAIN guest thread tid=(\d+)' | Select-Object -First 1 }
            else           { $m = $errText | Select-String "\[THREAD $gt\] host thread started.*hosttid=(\d+)" | Select-Object -First 1 }
            if ($m) { $hostTid = $m.Matches[0].Groups[1].Value }
            if ($hostTid) { [IO.File]::WriteAllText($dbg, "prof $hostTid 3`n"); Start-Sleep -Seconds 5 }
        }
    }
    $waitMs = [Math]::Max(1000, $waitMs - $ProfAt * 1000 - 5000 * $ProfTids.Count)
}
# -MonitorSec <n>: every n seconds ask the console for `stat` + `waits` (and `threads` every
# 4th time), appended to runs\<Tag>.dbg.txt -- so a play test that hangs and is closed by
# hand still leaves a record of where every guest thread was parked.
if ($MonitorSec -gt 0) {
    $deadline = (Get-Date).AddMilliseconds($waitMs); $k = 0
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
        if ($p.WaitForExit($MonitorSec * 1000)) { break }
        $cmds = @("stat", "waits", "spus"); if ((++$k % 4) -eq 0) { $cmds += "threads" }
        $cmds += $DbgExtra   # e.g. "mem <hex> <n>" sampled every cycle, not only at the kill
        # -MonitorProf: also sample the main and render threads (prof 3 s each) every cycle.
        if ($MonitorProf) {
            $errText = Get-Content $err -ErrorAction SilentlyContinue
            $m1 = $errText | Select-String '\[boot\] MAIN guest thread tid=(\d+)' | Select-Object -First 1
            $m7 = $errText | Select-String '\[THREAD 7\] host thread started.*hosttid=(\d+)' | Select-Object -First 1
            foreach ($m in $m1, $m7) { if ($m) { $cmds += "prof $($m.Matches[0].Groups[1].Value) 3" } }
        }
        # -ProfSpu <n>[,<n>...]: also sample SPU thread 0x200n (host tid from the
        # "[SPU] group_start ... tid=0x200n ... hosttid=" line) for 3 s every cycle.
        if ($ProfSpu.Count -gt 0) {
            $errText = Get-Content $err -ErrorAction SilentlyContinue
            foreach ($n in $ProfSpu) {
                $ms = $errText | Select-String ("\[SPU\] group_start .*tid=0x{0:X4} .*hosttid=(\d+)" -f (0x2000 + $n)) | Select-Object -First 1
                if ($ms) { $cmds += "prof $($ms.Matches[0].Groups[1].Value) 3" }
            }
        }
        foreach ($c in $cmds) { if ($p.HasExited) { break }; [IO.File]::WriteAllText($dbg, "$c`n")
                                Start-Sleep -Milliseconds $(if ($c -like 'prof*') { 4500 } else { 1500 }) }
    }
    $waitMs = [Math]::Max(1, [int]($deadline - (Get-Date)).TotalMilliseconds)
}
$exited = $p.WaitForExit($waitMs)
if (-not $exited) {
    foreach ($c in @("stat", "waits") + $DbgExtra) { [IO.File]::WriteAllText($dbg, "$c`n"); Start-Sleep -Milliseconds 1500 }
    Stop-Process -Id $p.Id -Force
    "killed after $Seconds s"
} else { "exited with code $($p.ExitCode)" }
foreach ($k in $Env.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
if (Test-Path "$dbg.out") { Move-Item "$dbg.out" "$logs\$Tag.dbg.txt" -Force }
Remove-Item $dbg -ErrorAction SilentlyContinue
"stdout lines: $((Get-Content $out | Measure-Object -Line).Lines)  stderr lines: $((Get-Content $err | Measure-Object -Line).Lines)"
if (Test-Path "$logs\$Tag.dbg.txt") { Get-Content "$logs\$Tag.dbg.txt" | Where-Object { $_ -match 'flips|uptime|tid ' } }
