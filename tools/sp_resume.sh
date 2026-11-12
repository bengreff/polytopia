#!/bin/bash
# Brings the Polytopia jobs on the PC back after the WSL distro restarts. A Windows sign-out ends the
# keepalive's wsl.exe session and WSL stops the distro with every job in it (2026-11-11 15:30: S11
# lost 4 hours mid-generation; self-play resumes from the games already on disk). Starts, each only
# if it isn't already running:
#   - the self-play watchdog (tools/sp_watchdog.sh, LIMIT 120 for planner v2's long games), unless
#     ~/runs/sp/WATCHDOG_STOP exists;
#   - the track rotation (tools/tracks.sh), unless ~/runs/sp/TRACKS_STOP exists.
# Run by hand after a sign-out or restart: bash ~/poly/py_sp/sp_resume.sh (DELAY=0 skips the wait).
# To run it at every distro start, greff's crontab in Ubuntu-22.04 would take
# "@reboot /bin/bash /home/greff/poly/py_sp/sp_resume.sh" (not installed: Ben's call). Logs to
# ~/runs/sp/tracks.log.
# The evening throttle is a Windows process (tools/throttle.ps1, started from ssh), not started here:
# a job in WSL can't change the VM's CPU affinity.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/lib/wsl/lib
R=/home/greff/runs/sp
P=/home/greff/poly/py_sp
log() { echo "$(date '+%F %T') resume: $*" >> $R/tracks.log; }
sleep ${DELAY:-90}  # the GPU driver and the keepalive come up after the sign-in
running() { pgrep -f "[/]home/greff/$1" > /dev/null; }
if [ ! -f $R/WATCHDOG_STOP ] && ! running poly/py_sp/sp_watchdog.sh; then
    LIMIT=120 setsid nohup $P/sp_watchdog.sh > /dev/null 2>&1 < /dev/null &
    log "watchdog started (LIMIT 120)"
fi
if [ -f $R/TRACKS_STOP ]; then
    log "TRACKS_STOP present: rotation not restarted"
elif ! running poly/py_sp/tracks.sh && ! pgrep -f "[s]p_loop.py $R/" > /dev/null; then
    setsid nohup $P/tracks.sh >> $R/tracks.out 2>&1 < /dev/null &
    log "rotation started (tracks: $(tr '\n' ' ' < $R/tracks.list))"
fi
sleep 5
