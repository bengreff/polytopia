# Registers the scheduled task PolytopiaTrainingStarter on the PC (Ben approved it, 2026-11-14): 2 minutes
# after Ben signs in, elevated and hidden, it runs tools/throttle.ps1 -Start (installed as
# C:\Users\ben\polytopia_throttle.ps1), which restarts the training only if some is pending and then
# throttles it. Run from an ssh session (elevated):
#   scp tools/throttle.ps1 backhouse:polytopia_throttle.ps1; scp tools/throttle_task.ps1 backhouse:
#   ssh backhouse "powershell -ExecutionPolicy Bypass -File throttle_task.ps1"
# Remove: Unregister-ScheduledTask PolytopiaTrainingStarter -Confirm:$false
$user = "$env:COMPUTERNAME\$env:USERNAME"
$a = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument '--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\Users\ben\polytopia_throttle.ps1 -Start'
$t = New-ScheduledTaskTrigger -AtLogOn -User $user
$t.Delay = 'PT2M'
$p = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName PolytopiaTrainingStarter -Action $a -Trigger $t -Principal $p -Settings $s -Force `
    -Description 'Polytopia training: after sign-in, restarts pending training (sp_resume.sh) and throttles the WSL VM 17:00-22:00 and while Roblox or Minecraft is open. Script: C:\Users\ben\polytopia_throttle.ps1' |
    Select-Object TaskName, State
