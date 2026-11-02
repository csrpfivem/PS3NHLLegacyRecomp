#!/bin/sh
# Deck: profile the next match with no timing by hand, inside one open ssh session; nothing is
# installed and nothing stays running. Waits for a new game log, then for 8 seconds in a row under
# 57 steps (live play; the loading dips before a match last 5), records 30 s with the deck6real
# counters, and turns the samples into text once the game has closed. Results go in /tmp for the
# caller to fetch and delete.
# usage (from the PC): cmd /c "ssh deck@<deck-ip> sh -s <tag> < deck_profmatch.sh"
main() {
    cd ~/Games/NHLLegacy || exit 1
    TAG=${1:-deckprof}
    START=$(date +%s)
    DEADLINE=$((START + 1800))
    echo "armed $TAG"

    LOG=""
    while [ -z "$LOG" ]; do
        [ "$(date +%s)" -lt "$DEADLINE" ] || { echo "no game started"; exit 1; }
        F=$(ls -t logs/game_*.err.log 2>/dev/null | head -1)
        [ -n "$F" ] && [ "$(stat -c %Y "$F")" -ge "$START" ] && LOG=$F
        sleep 1
    done
    echo "log $LOG"

    while :; do
        [ "$(date +%s)" -lt "$DEADLINE" ] || { echo "no match seen"; exit 1; }
        N=0
        for V in $(grep -a '\[HZ\]' "$LOG" | tail -8 | sed -n 's/.*| steps \([0-9]*\).*/\1/p'); do
            [ "$V" -ge 1 ] && [ "$V" -le 56 ] && N=$((N + 1))
        done
        [ "$N" -eq 8 ] && break
        sleep 1
    done

    PID=$(for p in $(pgrep -f 'NHLLegacy.exe'); do echo "$(ls /proc/$p/task 2>/dev/null | wc -l) $p"; done | sort -n | tail -1 | cut -d' ' -f2)
    [ -n "$PID" ] || { echo "game not running"; exit 1; }
    echo "recording pid $PID at $(grep -a '\[HZ\]' "$LOG" | tail -1 | cut -c1-14)"
    cat /proc/$PID/maps > /tmp/maps_$TAG.txt
    perf record -q -p "$PID" -e cycles:u,L1-dcache-load-misses:u,ic_fetch_stall.ic_stall_any:u -F 997 -o /tmp/perf_$TAG.data -- sleep 30 2>&1 | tail -3
    echo "recorded at $(grep -a '\[HZ\]' "$LOG" | tail -1 | cut -c1-14)"

    # perf script is heavy, so it waits for the game to close
    while kill -0 "$PID" 2>/dev/null && [ "$(date +%s)" -lt "$DEADLINE" ]; do sleep 2; done
    perf script -i /tmp/perf_$TAG.data -F tid,event,ip 2>/dev/null | awk '{c[$2" "$3" "$1]++} END {for (k in c) print c[k], k}' | sort -rn > /tmp/perfrec_$TAG.txt
    rm -f /tmp/perf_$TAG.data
    echo "DONE $(wc -l < /tmp/perfrec_$TAG.txt) lines, log $LOG"
}
main "$@" < /dev/null
