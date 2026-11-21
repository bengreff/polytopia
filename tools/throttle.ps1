# Polytopia's training starter on the PC (Windows side), and the Roblox closer. The file keeps its old name
# (throttle.ps1) so the scheduled task that runs it doesn't change.
#
# No throttle (Ben, 2026-11-21: "Remove any decision to throttle. Go full speed always. Disregard all roblox
# and close if necessary if there is no one at the keyboard."). Every minute it keeps the WSL VM on all 28
# logical CPUs at Normal priority, and if the Roblox game (RobloxPlayerBeta) is open with no keyboard or
# mouse input at the PC for 10 minutes, it closes it (a forced stop).
# Roblox Studio is left alone (it can hold unsaved work), and so is Minecraft. Until 2026-11-21 it held the
# VM on the 12 E-cores at BelowNormal from 17:00 to 22:00 and while Roblox or Minecraft was in use.
# Input can only be read by a process in the signed-in console session (the task's): a copy started any
# other way can't tell whether someone is at the PC and never closes Roblox.
# It exits once the training has not been running for 15 minutes.
#
# Starter (-Start): run by the scheduled task PolytopiaTrainingStarter 2 minutes after Ben signs in
# (elevated, hidden; registered by tools/throttle_task.ps1). It asks WSL whether any training is
# pending (tools/sp_resume.sh check: a track in ~/runs/sp/tracks.list below its LAST, without STOP,
# and no TRACKS_STOP). If none is, it does nothing. If one is, it runs sp_resume.sh (watchdog and
# rotation, each only if not running) and then keeps watch as above.
#
# By hand from ssh: Start-ScheduledTask PolytopiaTrainingStarter (needs Ben signed in), or, with nobody
# signed in, the WMI launch: Invoke-CimMethod Win32_Process Create -Arguments @{CommandLine=
# 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\ben\polytopia_throttle.ps1'}
# Only one copy runs at a time (a named mutex). Stop: create C:\Users\ben\polytopia_throttle.stop.
# Log: C:\Users\ben\polytopia_throttle.log; now: polytopia_throttle.status.
param([switch]$Start)
$dir = 'C:\Users\ben'
$log = "$dir\polytopia_throttle.log"
$status = "$dir\polytopia_throttle.status"
$wsl = "$env:WINDIR\System32\wsl.exe"
$resume = '/home/greff/poly/py_sp/sp_resume.sh'
$roblox = 'RobloxPlayerBeta'  # the game only: Roblox Studio can hold unsaved work
$idleLimit = 10  # minutes with no keyboard or mouse input = nobody at the keyboard (Ben's definition of in use, 2026-11-18)

Add-Type -Namespace Poly -Name Input -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
[DllImport("kernel32.dll")] public static extern uint WTSGetActiveConsoleSessionId();
public static double IdleMinutes() {
    var i = new LASTINPUTINFO(); i.cbSize = (uint)Marshal.SizeOf(i);
    if (!GetLastInputInfo(ref i)) return -1;
    return unchecked((uint)Environment.TickCount - i.dwTime) / 60000.0;
}
'@

function Log($m) { Add-Content $log "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m" }
function Wsl([string[]]$a) { & $wsl -d Ubuntu-22.04 -- @a 2>&1 | Out-Null; return $LASTEXITCODE }
# Minutes since the last keyboard or mouse input at the PC, or -1 if this process can't tell: only a
# process in the signed-in console session sees it (the starter task runs there). `query user` reports
# the console's idle time as the time since sign-in (4 days on 2026-11-18 while Roblox ran), so it isn't used.
function Idle {
    if ((Get-Process -Id $PID).SessionId -ne [Poly.Input]::WTSGetActiveConsoleSessionId()) { return -1 }
    return [Poly.Input]::IdleMinutes()
}
# Closes the Roblox game if it's open and nobody has touched the keyboard or mouse for $idleLimit minutes.
# Returns a note for the status file.
$script:blind = $false
function CloseIdleRoblox {
    $p = @(Get-Process -Name $roblox -ErrorAction SilentlyContinue)
    if (-not $p) { return '' }
    $i = Idle
    if ($i -lt 0) {
        if (-not $script:blind) { Log "Roblox open, but this copy can't see keyboard or mouse input: left open" }
        $script:blind = $true
        return 'Roblox(open,idle-unknown)'
    }
    if ($i -lt $idleLimit) { return "Roblox(in-use,idle-$([int]$i)-min)" }
    # A forced stop: closing its window makes Roblox relaunch itself to the tray (--launch-to-tray, 2026-11-21),
    # and it keeps nothing on the PC that a close would save.
    $p | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep 3
    $left = @(Get-Process -Name $roblox -ErrorAction SilentlyContinue)
    Log "closed Roblox (pid $($p.Id -join ',')): no keyboard or mouse input for $([int]$i) min$(if ($left) { "; still open: pid $($left.Id -join ',')" })"
    return 'Roblox(closed)'
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
    Log "already running: this one (pid $PID) exits"
    if ($Start) { Resume }
    exit
}

Log "started (pid $PID): no throttle, VM at full speed; closes Roblox after $idleLimit min with no input"
$failed = $false; $idle = 0; $k = 0
while (-not (Test-Path "$dir\polytopia_throttle.stop")) {
    try {
        $out = Apply 0x0FFFFFFF 'Normal'
        if ($failed) { Log "VM back to full -> $out" }
        $failed = $false
    } catch {
        $out = "FAILED: $($_.Exception.Message)"
        if (-not $failed) { Log "setting the VM to full $out" }
        $failed = $true
    }
    $note = CloseIdleRoblox
    Set-Content $status "full $(Get-Date -Format 'HH:mm:ss') $note $out"
    if ($Start) { Resume; $Start = $false }
    if ($k % 5 -eq 0) {  # every 5 minutes: is the training still running?
        if ((Wsl 'bash', $resume, 'status') -eq 0) { $idle = 0 } else { $idle++ }
        if ($idle -ge 3) {
            try { $out = Apply 0x0FFFFFFF 'Normal' } catch { $out = "FAILED: $($_.Exception.Message)" }
            Log "training not running for 15 minutes: VM at full ($out); exits"
            Set-Content $status "full $(Get-Date -Format 'HH:mm:ss') exited $out"
            exit
        }
    }
    $k++
    Start-Sleep 60
}
Log 'stopped (stop file)'
