#!/bin/bash
# Brings the Polytopia training on the PC back after a restart, a power cut or a sign-out (each stops
# the WSL distro with every job in it; self-play resumes from the games already on disk).
#
#   sp_resume.sh          start, only if training is pending: the self-play watchdog (tools/sp_watchdog.sh,
#                         LIMIT 120 for planner v2's long games; not with ~/runs/sp/WATCHDOG_STOP) and the
#                         track rotation (tools/tracks.sh), each only if it isn't already running
#   sp_resume.sh check    exit 0 if training is pending: no ~/runs/sp/TRACKS_STOP, and a track in
#                         tracks.list without STOP that hasn't reached its LAST generation
#   sp_resume.sh status   exit 0 if the rotation or an sp_loop is running
#
# Run by the Windows task PolytopiaTrainingStarter after Ben signs in (tools/throttle.ps1 -Start, which
# then throttles the VM), or by hand: DELAY=0 bash ~/poly/py_sp/sp_resume.sh. Logs to ~/runs/sp/tracks.log.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/lib/wsl/lib
R=/home/greff/runs/sp
P=/home/greff/poly/py_sp
log() { echo "$(date '+%F %T') resume: $*" >> $R/tracks.log; }
running() { pgrep -f "[/]home/greff/$1" > /dev/null; }
training() { running poly/py_sp/tracks.sh || pgrep -f "[s]p_loop.py $R/" > /dev/null; }
pending() {
    [ -f $R/TRACKS_STOP ] && return 1
    for t in $(cat $R/tracks.list 2>/dev/null); do
        { [ -d $R/$t ] && [ ! -f $R/$t/STOP ]; } || continue
        g=0
        while [ -e $(printf "$R/$t/gen%03d.pt" $((g + 1))) ]; do g=$((g + 1)); done
        [ -f $R/$t/LAST ] && [ $g -ge $(cat $R/$t/LAST) ] && continue
        return 0
    done
    return 1
}
case ${1:-start} in
    check) pending; exit $? ;;
    status) training; exit $? ;;
esac
sleep ${DELAY:-90}  # the GPU driver and the keepalive come up after the sign-in
if ! pending; then
    log "no training pending (tracks done or stopped): nothing started"
    exit 0
fi
if [ ! -f $R/WATCHDOG_STOP ] && ! running poly/py_sp/sp_watchdog.sh; then
    LIMIT=120 setsid nohup $P/sp_watchdog.sh > /dev/null 2>&1 < /dev/null &
    log "watchdog started (LIMIT 120)"
fi
if ! training; then
    setsid nohup $P/tracks.sh >> $R/tracks.out 2>&1 < /dev/null &
    log "rotation started (tracks: $(tr '\n' ' ' < $R/tracks.list))"
fi
sleep 5
