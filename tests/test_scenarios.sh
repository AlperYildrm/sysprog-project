#!/usr/bin/env bash
#
# test_scenarios.sh - automated scenarios for the Watchdog project.
#
# Run from anywhere:  ./tests/test_scenarios.sh      (or: make test)
#
# The script builds the project, then starts a fresh ./watchdog for each
# scenario and checks its children (found with `ps` by parent PID) and
# watchdog.log. It does not depend on the exact wording of process_manager's
# log messages, except where noted.
#
# Assumptions (override with environment variables):
#   WATCHDOG   path of the watchdog binary         (default ./watchdog)
#   WD_ARGS    arguments for the watchdog          (default: none)
#   LOG        log file the watchdog writes        (default watchdog.log)
#   STARTUP    seconds to wait after start         (default 2)
#   SAMPLES    directory for saved sample logs     (default tests/sample_logs)
#
# The watchdog's own configuration should start at least two `worker daemon`
# children; a `worker normal <sec>` child enables test 2 and a
# `worker stubborn` child makes test 6 exercise the SIGKILL fallback.

set -u

cd "$(dirname "$0")/.." || exit 1

WATCHDOG=${WATCHDOG:-./watchdog}
WD_ARGS=${WD_ARGS:-}
LOG=${LOG:-watchdog.log}
STARTUP=${STARTUP:-2}
SAMPLES=${SAMPLES:-tests/sample_logs}

PASS=0
FAIL=0
SKIP=0
WD_PID=""

if [ -t 1 ]; then
    GREEN=$'\e[32m'; RED=$'\e[31m'; YELLOW=$'\e[33m'; BOLD=$'\e[1m'; RESET=$'\e[0m'
else
    GREEN=""; RED=""; YELLOW=""; BOLD=""; RESET=""
fi

pass() { echo "  ${GREEN}PASS${RESET} $*"; PASS=$((PASS + 1)); }
fail() { echo "  ${RED}FAIL${RESET} $*"; FAIL=$((FAIL + 1)); }
skip() { echo "  ${YELLOW}SKIP${RESET} $*"; SKIP=$((SKIP + 1)); }
title() { echo; echo "${BOLD}== Test $* ==${RESET}"; }

# --- helpers ---------------------------------------------------------------

# PIDs of the watchdog's live (non-zombie) children whose command line matches $1.
children() {
    ps -eo pid=,ppid=,stat=,args= |
        awk -v p="$WD_PID" -v pat="$1" '$2 == p && $3 !~ /^Z/ && $0 ~ pat { print $1 }'
}

# Number of zombie (defunct) children of PID $1.
zombies_of() {
    ps -eo ppid=,stat= | awk -v p="$1" '$1 == p && $2 ~ /^Z/' | wc -l
}

start_watchdog() {
    rm -f "$LOG"
    # shellcheck disable=SC2086
    "$WATCHDOG" $WD_ARGS >/dev/null 2>&1 &
    WD_PID=$!
    sleep "$STARTUP"
    if ! kill -0 "$WD_PID" 2>/dev/null; then
        echo "  ${RED}watchdog exited right after start${RESET}"
        return 1
    fi
    return 0
}

# Stop the watchdog with signal $1 (default TERM); return 0 if it exits in time.
stop_watchdog() {
    local sig=${1:-TERM} i
    [ -n "$WD_PID" ] || return 0
    kill -"$sig" "$WD_PID" 2>/dev/null
    for i in $(seq 1 100); do                 # up to 10 s
        kill -0 "$WD_PID" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$WD_PID" 2>/dev/null; then
        kill -KILL "$WD_PID" 2>/dev/null
        wait "$WD_PID" 2>/dev/null
        WD_PID=""
        return 1
    fi
    wait "$WD_PID" 2>/dev/null
    WD_PID=""
    return 0
}

save_log() {
    mkdir -p "$SAMPLES"
    [ -f "$LOG" ] && cp "$LOG" "$SAMPLES/$1.txt"
}

cleanup() {
    [ -n "$WD_PID" ] && stop_watchdog KILL
    pkill -KILL -x worker 2>/dev/null
}
trap cleanup EXIT

# --- build -----------------------------------------------------------------

echo "${BOLD}Building...${RESET}"
if ! make >/dev/null; then
    echo "${RED}Build failed, aborting.${RESET}"
    exit 1
fi
if [ ! -x "$WATCHDOG" ] || [ ! -x ./worker ]; then
    echo "${RED}watchdog or worker binary missing, aborting.${RESET}"
    exit 1
fi

# --- Test 1: at least two workers start and their PIDs are logged ----------

title "1: at least two workers start, PIDs logged"
if start_watchdog; then
    pids=$(children 'worker')
    n=$(echo "$pids" | grep -c .)
    if [ "$n" -ge 2 ]; then
        pass "$n workers running: $(echo $pids)"
    else
        fail "only $n worker(s) running"
    fi
    missing=""
    for p in $pids; do
        grep -qw "$p" "$LOG" || missing="$missing $p"
    done
    if [ -z "$missing" ] && [ -n "$pids" ]; then
        pass "every worker PID appears in $LOG"
    else
        fail "PIDs missing from $LOG:${missing:- (no workers)}"
    fi
    save_log test1_startup
    stop_watchdog TERM
fi

# --- Test 2: a worker that exits with 0 is not restarted -------------------

title "2: normal exit (code 0) is not restarted"
if start_watchdog; then
    normal=$(children 'worker normal')
    if [ -z "$normal" ]; then
        skip "no 'worker normal <sec>' child in the watchdog configuration"
    else
        secs=$(ps -o args= -p "$(echo "$normal" | head -1)" | awk '{print $3}')
        sleep $((secs + 2))
        if [ -z "$(children 'worker normal')" ]; then
            pass "normal worker(s) exited and were not restarted"
        else
            fail "a 'worker normal' child is still running after ${secs}s"
        fi
        if grep -Eiq '(exit|status|code)[^0-9]*0([^0-9]|$)' "$LOG"; then
            pass "exit code 0 is logged"
        else
            fail "no exit-code-0 line in $LOG"
        fi
    fi
    save_log test2_normal_exit
    stop_watchdog TERM
fi

# --- Test 3: a worker killed with SIGKILL is restarted ---------------------

title "3: SIGKILL'd worker is restarted"
if start_watchdog; then
    before=$(children 'worker daemon' | sort)
    victim=$(echo "$before" | head -1)
    if [ -z "$victim" ]; then
        fail "no 'worker daemon' child to kill"
    else
        kill -KILL "$victim"
        sleep 2
        after=$(children 'worker daemon' | sort)
        new=$(comm -13 <(echo "$before") <(echo "$after"))
        if [ -n "$new" ] && ! echo "$after" | grep -qx "$victim"; then
            pass "PID $victim was replaced by PID $new"
        else
            fail "no replacement worker after killing PID $victim"
        fi
        if grep -iq 'restart' "$LOG"; then
            pass "restart is logged"
        else
            fail "no 'restart' line in $LOG"
        fi
    fi
    save_log test3_restart
    stop_watchdog TERM
fi

# --- Test 4: after 3 restarts the worker is given up on --------------------

title "4: kill the same worker 4 times -> at most 3 restarts"
if start_watchdog; then
    current=$(children 'worker daemon' | sort | head -1)
    restarts=0
    for round in 1 2 3 4; do
        [ -n "$current" ] || break
        before=$(children 'worker daemon' | sort)
        kill -KILL "$current"
        sleep 2
        after=$(children 'worker daemon' | sort)
        current=$(comm -13 <(echo "$before") <(echo "$after") | head -1)
        if [ -n "$current" ]; then
            restarts=$((restarts + 1))
            echo "  kill #$round -> restarted as PID $current"
        else
            echo "  kill #$round -> not restarted"
        fi
    done
    if [ "$restarts" -eq 3 ]; then
        pass "worker was restarted exactly 3 times, then left stopped"
    else
        fail "expected 3 restarts, saw $restarts"
    fi
    if grep -Eiq 'limit' "$LOG"; then
        pass "restart limit is logged"
    else
        fail "no 'limit' line in $LOG"
    fi
    if kill -0 "$WD_PID" 2>/dev/null; then
        pass "watchdog keeps running with the other workers"
    else
        fail "watchdog died"
    fi
    save_log test4_restart_limit
    stop_watchdog TERM
fi

# --- Test 5: SIGUSR1 writes a status report --------------------------------

title "5: SIGUSR1 status report"
if start_watchdog; then
    kill -USR1 "$WD_PID"
    sleep 1
    if grep -q 'SIGUSR1 Process Status Report' "$LOG"; then
        pass "report header found in $LOG"
    else
        fail "no status report in $LOG"
    fi
    rows=$(grep -c '\[STATUS\] Name: .* | PID: .* | State: .* | Restarts: [0-9]*/[0-9]*' "$LOG")
    if [ "$rows" -ge 2 ]; then
        pass "$rows child rows in the report"
    else
        fail "expected at least 2 child rows, found $rows"
    fi
    if kill -0 "$WD_PID" 2>/dev/null; then
        pass "watchdog still running after SIGUSR1"
    else
        fail "watchdog died on SIGUSR1"
    fi
    save_log test5_status_report
    stop_watchdog TERM
fi

# --- Test 6: SIGTERM and SIGINT shut down cleanly, no zombies --------------

for sig in TERM INT; do
    title "6: graceful shutdown on SIG$sig"
    if start_watchdog; then
        wd=$WD_PID
        kids=$(children 'worker')
        if stop_watchdog "$sig"; then
            pass "watchdog exited after SIG$sig"
        else
            fail "watchdog did not exit within 10 s of SIG$sig"
        fi
        alive=""
        for p in $kids; do
            kill -0 "$p" 2>/dev/null && alive="$alive $p"
        done
        if [ -z "$alive" ]; then
            pass "all $(echo "$kids" | grep -c .) workers are gone"
        else
            fail "workers still alive:$alive"
        fi
        z=$(zombies_of "$wd")
        if [ "$z" -eq 0 ] && ! ps -eo stat=,args= | grep -q '^Z.*worker'; then
            pass "no defunct (zombie) workers"
        else
            fail "defunct workers found"
        fi
        save_log "test6_shutdown_sig$(echo "$sig" | tr '[:upper:]' '[:lower:]')"
    fi
done

# --- Test 7: every log line is timestamped ---------------------------------

title "7: every log line has [YYYY-MM-DD HH:MM:SS] [LEVEL]"
if start_watchdog; then
    kill -USR1 "$WD_PID"
    sleep 1
    stop_watchdog TERM
    total=$(grep -c . "$LOG")
    bad=$(grep -Evc '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] \[[A-Z]+\] ' "$LOG")
    if [ "$total" -gt 0 ] && [ "$bad" -eq 0 ]; then
        pass "all $total lines match the format"
    else
        fail "$bad of $total lines do not match the format"
        grep -Ev '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] \[[A-Z]+\] ' "$LOG" | head -5
    fi
    save_log test7_full_run
fi

# --- summary ---------------------------------------------------------------

echo
echo "${BOLD}Summary:${RESET} ${GREEN}$PASS passed${RESET}, ${RED}$FAIL failed${RESET}, ${YELLOW}$SKIP skipped${RESET}"
echo "Sample logs saved in $SAMPLES/"
[ "$FAIL" -eq 0 ]
