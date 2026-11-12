# Evening throttle for the WSL VM on the PC (Ben's sharing rule): 17:00-22:00 the VM runs on the 12
# E-core logical CPUs (16-27) at BelowNormal priority; all 28 logical CPUs at Normal otherwise.
# Re-applied every 5 minutes. Replaces ~/throttle/throttled.sh in WSL: changing vmmemWSL's affinity
# needs an elevated process, and a detached WSL job only reaches Windows through the keepalive's
# unelevated session ("Access is denied"; once the ssh session that started it ends, its own route
# fails too: "UtilAcceptVsock: accept4 failed 110"). So on 2026-11-10 the VM was pinned at 19:52 and
# never unpinned at 22:00; S11 ran on the E-cores alone for 19 hours.
#
# Start it from an ssh session (elevated), so it outlives the session (dies at a Windows restart):
#   scp tools/throttle.ps1 backhouse:polytopia_throttle.ps1
#   ssh backhouse "powershell -NoProfile -Command \"(Invoke-CimMethod Win32_Process Create -Arguments @{CommandLine='powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\ben\polytopia_throttle.ps1'}).ProcessId\""
# Stop it: create C:\Users\ben\polytopia_throttle.stop (it exits within 5 minutes), or stop its PID.
# Log: C:\Users\ben\polytopia_throttle.log (mode changes and failures); now: polytopia_throttle.status.
$dir = 'C:\Users\ben'
$log = "$dir\polytopia_throttle.log"
$last = ''
$failed = $false
Add-Content $log "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') started (pid $PID)"
while (-not (Test-Path "$dir\polytopia_throttle.stop")) {
    $h = (Get-Date).Hour
    if ($h -ge 17 -and $h -lt 22) { $mode = 'peak'; $mask = 0x0FFF0000; $pri = 'BelowNormal' }
    else { $mode = 'full'; $mask = 0x0FFFFFFF; $pri = 'Normal' }
    try {
        $p = Get-Process vmmemWSL -ErrorAction Stop
        $p.ProcessorAffinity = [IntPtr]$mask
        $p.PriorityClass = $pri
        $out = '{0} 0x{1:X}' -f $p.PriorityClass, [int64]$p.ProcessorAffinity
        if ($mode -ne $last) { Add-Content $log "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') mode $mode -> $out" }
        $last = $mode
        $failed = $false
    } catch {
        $out = "FAILED: $($_.Exception.Message)"
        if (-not $failed) { Add-Content $log "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') mode $mode $out" }
        $last = ''
        $failed = $true
    }
    Set-Content "$dir\polytopia_throttle.status" "$mode $(Get-Date -Format 'HH:mm:ss') $out"
    Start-Sleep 300
}
Add-Content $log "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') stopped (stop file)"
