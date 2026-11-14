# Polytopia's training starter and throttle on the PC (Windows side; Ben, 2026-11-14: "add the starter,
# make sure it throttles when something else is running (if minecraft or roblox is on) and only starts
# when we have training we are doing").
#
# Throttle: the WSL VM runs on the 12 E-core logical CPUs (16-27) at BelowNormal priority from 17:00 to
# 22:00, and at any hour while Roblox or Minecraft is open; on all 28 logical CPUs at Normal priority
# otherwise. Checked every minute. It exits, giving the VM the full CPU back, once the training has
# not been running for 15 minutes. It has to be a Windows process: changing vmmemWSL's affinity needs
# an elevated process, which a detached WSL job can't reach (2026-11-10: the WSL throttle pinned the VM
# at 19:52 and never unpinned it).
#
# Starter (-Start): run by the scheduled task PolytopiaTrainingStarter 2 minutes after Ben signs in
# (elevated, hidden; registered by tools/throttle_task.ps1). It asks WSL whether any training is
# pending (tools/sp_resume.sh check: a track in ~/runs/sp/tracks.list below its LAST, without STOP,
# and no TRACKS_STOP). If none is, it does nothing. If one is, it runs sp_resume.sh (watchdog and
# rotation, each only if not running) and then throttles.
#
# By hand from ssh: Start-ScheduledTask PolytopiaTrainingStarter (needs Ben signed in), or, with nobody
# signed in, the WMI launch: Invoke-CimMethod Win32_Process Create -Arguments @{CommandLine=
# 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\ben\polytopia_throttle.ps1'}
# Only one throttle runs at a time (a named mutex). Stop: create C:\Users\ben\polytopia_throttle.stop.
# Log: C:\Users\ben\polytopia_throttle.log; now: polytopia_throttle.status (read by sp_loop.free_procs).
param([switch]$Start)
$dir = 'C:\Users\ben'
$log = "$dir\polytopia_throttle.log"
$status = "$dir\polytopia_throttle.status"
$wsl = "$env:WINDIR\System32\wsl.exe"
$resume = '/home/greff/poly/py_sp/sp_resume.sh'
$games = 'RobloxPlayerBeta', 'RobloxStudioBeta', 'Minecraft.Windows'  # plus Java Minecraft (javaw with "minecraft" in its command line)

function Log($m) { Add-Content $log "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m" }
function Wsl([string[]]$a) { & $wsl -d Ubuntu-22.04 -- @a 2>&1 | Out-Null; return $LASTEXITCODE }
function Game {
    $p = Get-Process -Name $games -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) { return $p.ProcessName }
    $j = Get-CimInstance Win32_Process -Filter "Name = 'javaw.exe' OR Name = 'java.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match 'minecraft' } | Select-Object -First 1
    if ($j) { return 'Minecraft' }
    return ''
}
function Apply($mask, $pri) {
    $p = Get-Process vmmemWSL -ErrorAction Stop
    $p.ProcessorAffinity = [IntPtr]$mask
    $p.PriorityClass = $pri
    return '{0} 0x{1:X}' -f $p.PriorityClass, [int64]$p.ProcessorAffinity
}

function Resume { Wsl 'env', 'DELAY=0', 'bash', $resume | Out-Null; Log 'start: training pending: sp_resume.sh run' }
if ($Start -and (Wsl 'bash', $resume, 'check') -ne 0) { Log 'start: no training pending (tracks done or stopped): nothing started'; exit }
$mutex = New-Object System.Threading.Mutex($false, 'Global\PolytopiaThrottle')
try { $mine = $mutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $mine = $true }
if (-not $mine) {
    Log "throttle already running: this one (pid $PID) exits"
    if ($Start) { Resume }
    exit
}

Log "throttle started (pid $PID)"
$last = ''; $failed = $false; $idle = 0; $k = 0
while (-not (Test-Path "$dir\polytopia_throttle.stop")) {
    $h = (Get-Date).Hour
    $g = Game
    if ($h -ge 17 -and $h -lt 22) { $why = 'evening' } elseif ($g) { $why = $g } else { $why = '' }
    if ($why) { $mode = 'peak'; $mask = 0x0FFF0000; $pri = 'BelowNormal' } else { $mode = 'full'; $mask = 0x0FFFFFFF; $pri = 'Normal' }
    try {
        $out = Apply $mask $pri
        if ("$mode $why" -ne $last) { Log "mode $mode $why -> $out" }
        $last = "$mode $why"; $failed = $false
    } catch {
        $out = "FAILED: $($_.Exception.Message)"
        if (-not $failed) { Log "mode $mode $why $out" }
        $last = ''; $failed = $true
    }
    Set-Content $status "$mode $(Get-Date -Format 'HH:mm:ss') $why $out"
    if ($Start) { Resume; $Start = $false }  # after the first status, so sp_loop's free_procs sees this mode
    if ($k % 5 -eq 0) {  # every 5 minutes: is the training still running?
        if ((Wsl 'bash', $resume, 'status') -eq 0) { $idle = 0 } else { $idle++ }
        if ($idle -ge 3) {
            try { $out = Apply 0x0FFFFFFF 'Normal' } catch { $out = "FAILED: $($_.Exception.Message)" }
            Log "training not running for 15 minutes: VM back to full ($out); throttle exits"
            Set-Content $status "full $(Get-Date -Format 'HH:mm:ss') exited $out"
            exit
        }
    }
    $k++
    Start-Sleep 60
}
Log 'stopped (stop file)'
