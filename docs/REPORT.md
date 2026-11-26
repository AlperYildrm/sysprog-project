---
title: "Watchdog: A POSIX Signal-Based Process Monitor"
subtitle: "BSM308 / SWE-301 System Programming, Term Project 1"
---

**Sakarya University, November 2026**

**Team:**

| Student | Student no. | GitHub |
|---|---|---|
| Alper Yıldırım | B231202001 | AlperYildrm |
| Ammar Al Baghajati | B231202553 | Ammarito9 |
| Ziyad Alboghdady | B231202560 | ziyad-alboghdady |

**Public repository:** <https://github.com/AlperYildrm/sysprog-project>

# 1. Introduction

`watchdog` is a supervisor process for Linux/POSIX. It starts a fixed set of
worker processes with `fork()` and `execv()`, learns that a worker ended through
SIGCHLD, restarts failed workers up to three times each, writes a status report
of every worker when it receives SIGUSR1, and on SIGTERM or SIGINT (Ctrl+C) stops
all workers without leaving zombie processes. Every event is written to
`watchdog.log` with a timestamp.

The whole program is built with `make` and compiles with
`-Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L` without a single
warning.

# 2. Architecture

```
            SIGCHLD   SIGALRM   SIGUSR1   SIGTERM / SIGINT
               |         |         |             |
        +------v---------v---------v-------------v-----+
        |   signal_manager: handlers only set flags    |
        +----------------------+-----------------------+
                               |  volatile sig_atomic_t flags
        +----------------------v-----------------------+
        | main.c: block signals -> check flags ->      |
        |         sigsuspend() -> repeat               |
        +-------+----------------+----------------+----+
                |                |                |
  handle_child_terminations()  periodic_      log_status_report()
                |           process_check()       |
        +-------v----------------v------+   +-----v------+
        |        process_manager        |-->|   logger   |--> watchdog.log
        |  child table, fork/execv,     |   +------------+
        |  waitpid, restart policy      |
        +-------+-----------------------+
                |  fork + execv
     +----------v--+ +-------------+ +--------------+ +---------------+
     | worker_1    | | worker_2    | | worker_3     | | worker_4      |
     | daemon      | | daemon      | | normal 10    | | stubborn      |
     +-------------+ +-------------+ +--------------+ +---------------+
```

The program has four modules with fixed interfaces in `include/`:

| Module | Files | Responsibility |
|---|---|---|
| Signal manager | `signal_manager.h/.c` | installs handlers with `sigaction()`, exposes the four flags, blocks and restores the managed signal mask |
| Main loop | `main.c` | start-up, event loop with `sigsuspend()`, graceful shutdown |
| Process manager | `process_manager.h/.c` | child table, spawning, reaping, restart policy, health check timer |
| Logger | `logger.h/.c` | timestamped log file, SIGUSR1 status report |
| Worker | `workers/worker.c` | test child with four behaviours |

## 2.1 Signal handling

Handlers for SIGCHLD, SIGUSR1, SIGALRM, SIGTERM and SIGINT are installed with
`sigaction()`. Each handler saves `errno`, sets one `volatile sig_atomic_t` flag
(`g_child_terminated`, `g_status_requested`, `g_alarm_triggered`,
`g_shutdown_requested`) and restores `errno`. No handler calls a function that
is not async-signal-safe. SIGCHLD uses `SA_NOCLDSTOP`, so a stopped child does
not wake the watchdog.

The main loop keeps the managed signals blocked while it checks the flags, then
calls `sigsuspend()` with the old mask. `sigsuspend()` unblocks the signals and
sleeps in one atomic step, so a signal that arrives between "check flags" and
"sleep" is delivered during the sleep and cannot be lost. The loop uses no CPU
while it waits (no busy waiting).

## 2.2 Child process management

Each child is a `ChildProcess` entry in a table of at most 16 (`MAX_CHILDREN`):
PID, name, executable, a copy of its arguments, state, restart count and last
start time. The child side of `fork()` resets the five managed signals to
`SIG_DFL` and clears the signal mask before `execv()`, so a worker does not
inherit the watchdog's handlers or blocked signals.

On SIGCHLD, `handle_child_terminations()` calls `waitpid(-1, &status, WNOHANG)`
in a loop until no finished child is left, because several SIGCHLD signals can
merge into one. The status is decoded with `WIFEXITED`/`WEXITSTATUS` and
`WIFSIGNALED`/`WTERMSIG`:

| How the child ended | New state | Action |
|---|---|---|
| exit code 0 | `EXITED_NORMAL` | not restarted |
| non-zero exit code | `TERMINATED_ERROR` | restarted if `restart_count < 3` |
| killed by a signal | `TERMINATED_ERROR` | restarted if `restart_count < 3` |
| fourth failure | `RESTART_LIMIT_REACHED` | left stopped, limit logged |

A periodic health check runs every 4 seconds: `setitimer(ITIMER_REAL)` raises
SIGALRM, and `periodic_process_check()` reaps any child whose SIGCHLD was missed,
checks each running PID with `kill(pid, 0)` and logs the running PIDs.

## 2.3 Graceful shutdown

On SIGTERM or SIGINT the loop ends and the watchdog:

1. calls `set_shutdown_mode(1)`, so no child is restarted or started any more;
2. sends SIGTERM to every running child;
3. reaps children for up to 5 seconds;
4. sends SIGKILL to any child still alive (for example `worker stubborn`, which
   ignores SIGTERM) and reaps it;
5. collects any remaining child with `waitpid(-1, ..., WNOHANG)`, logs
   "Shutdown complete" and closes the log.

## 2.4 Logging and status report

The log is opened with `open("watchdog.log", O_WRONLY | O_CREAT | O_APPEND, 0644)`
and marked `FD_CLOEXEC` so workers do not inherit it. Each line is built in one
buffer (`time()`, `localtime_r()` and `strftime()` for the timestamp, `vsnprintf()`
for the message) and written with a single `write()`, so a line is never split or
left in a stdio buffer if the process is killed. Every line has the form
`[YYYY-MM-DD HH:MM:SS] [LEVEL] message`, with the levels INFO, WARN, ERROR and
STATUS. The logger is only called from the main loop, never from a handler.

`log_status_report()` reads the child table through `get_process_list()` and
`process_state_to_string()` and writes a header, one line per child
(name, PID, state, restarts) and a closing line.

## 2.5 Worker program

`worker` lets every scenario be reproduced:

| Command | Behaviour |
|---|---|
| `worker daemon` | runs forever in a `pause()` loop; default action on SIGTERM |
| `worker normal <sec>` | sleeps `<sec>` seconds, exits with code 0 |
| `worker crash <sec> [abort]` | sleeps `<sec>` seconds, exits with code 1 (or `abort()`) |
| `worker stubborn` | ignores SIGTERM, only SIGKILL ends it |

The watchdog starts `worker_1` and `worker_2` (daemon), `worker_3` (normal 10)
and `worker_4` (stubborn).

# 3. Task distribution

| Student | Student no. | GitHub | Issues | Files |
|---|---|---|---|---|
| S1 Alper Yıldırım | B231202001 | AlperYildrm | #1-#4, #16 | `include/signal_manager.h`, `src/signal_manager.c`, `src/main.c` |
| S2 Ammar Al Baghajati | B231202553 | Ammarito9 | #5-#9, #16 | `include/process_manager.h`, `src/process_manager.c` |
| S3 Ziyad Alboghdady | B231202560 | ziyad-alboghdady | #10-#15, #16 | `include/logger.h`, `src/logger.c`, `workers/worker.c`, `Makefile`, `tests/test_scenarios.sh`, `README.md`, `docs/REPORT.pdf` |

Each module was developed on its own branch by its owner and merged into `main`
through a pull request (#17-#21 and later).

# 4. Building and running

```
make                               # builds ./watchdog and ./worker, no warnings
./watchdog                         # run from the project root; log in ./watchdog.log
kill -USR1 $(pgrep -x watchdog)    # status report
kill -TERM $(pgrep -x watchdog)    # graceful shutdown (or Ctrl+C)
make test                          # automated scenarios
make clean
```

# 5. Tests

`tests/test_scenarios.sh` (also `make test`) builds the project, starts a fresh
watchdog for each scenario, checks the watchdog's children with `ps` and checks
`watchdog.log`. It saves the log of each scenario in `tests/sample_logs/`.

| # | Scenario | How it is checked | Result |
|---|---|---|---|
| 1 | At least 2 workers start | 4 children of the watchdog in `ps`, each PID in the log | PASS |
| 2 | Exit code 0 is not restarted | `worker normal` is gone after its time, exit code 0 logged | PASS |
| 3 | SIGKILL'd worker is restarted | killed PID replaced by a new worker PID, restart logged | PASS |
| 4 | 4 kills, at most 3 restarts | 3 replacements, the 4th kill is not restarted, "limit" logged | PASS |
| 5 | SIGUSR1 status report | header and one row per child in the log | PASS |
| 6 | SIGTERM and Ctrl+C | watchdog exits, all workers gone, no `Z` (defunct) process | PASS |
| 7 | Timestamps | every line matches `[YYYY-MM-DD HH:MM:SS] [LEVEL]` | PASS |

Result of the final run on the integrated `main` branch: **19 checks passed,
0 failed, 0 skipped**.

## 5.1 Test script output

::: term
```
Building...

== Test 1: at least two workers start, PIDs logged ==
  PASS 4 workers running: 1137 1138 1139 1140
  PASS every worker PID appears in watchdog.log

== Test 2: normal exit (code 0) is not restarted ==
  PASS normal worker(s) exited and were not restarted
  PASS exit code 0 is logged

== Test 3: SIGKILL'd worker is restarted ==
  PASS PID 1284 was replaced by PID 1297
  PASS restart is logged

== Test 4: kill the same worker 4 times -> at most 3 restarts ==
  kill #1 -> restarted as PID 1381
  kill #2 -> restarted as PID 1398
  kill #3 -> restarted as PID 1415
  kill #4 -> not restarted
  PASS worker was restarted exactly 3 times, then left stopped
  PASS restart limit is logged
  PASS watchdog keeps running with the other workers

== Test 5: SIGUSR1 status report ==
  PASS report header found in watchdog.log
  PASS 4 child rows in the report
  PASS watchdog still running after SIGUSR1

== Test 6: graceful shutdown on SIGTERM ==
  PASS watchdog exited after SIGTERM
  PASS all 4 workers are gone
  PASS no defunct (zombie) workers

== Test 6: graceful shutdown on SIGINT ==
  PASS watchdog exited after SIGINT
  PASS all 4 workers are gone
  PASS no defunct (zombie) workers

== Test 7: every log line has [YYYY-MM-DD HH:MM:SS] [LEVEL] ==
  PASS all 29 lines match the format

Summary: 19 passed, 0 failed, 0 skipped
Sample logs saved in tests/sample_logs/
```
:::
*Figure 1: terminal output of `./tests/test_scenarios.sh` on the integrated code.*

## 5.2 Manual run

::: term
```
$ ps -o pid,ppid,stat,args --ppid 1861
  PID  PPID STAT COMMAND
 1863  1861 S    ./worker daemon
 1864  1861 S    ./worker daemon
 1865  1861 S    ./worker normal 10
 1866  1861 S    ./worker stubborn
$ kill -9 1863
$ ps -o pid,ppid,stat,args --ppid 1861
  PID  PPID STAT COMMAND
 1864  1861 S    ./worker daemon
 1865  1861 S    ./worker normal 10
 1866  1861 S    ./worker stubborn
 1875  1861 S    ./worker daemon
```
:::
*Figure 2: the four workers as children of the watchdog; after `kill -9` the
daemon worker is replaced by a new PID.*

::: term
```
$ ./watchdog
[2026-11-25 20:14:21] [INFO] Watchdog started (PID 1861)
[2026-11-25 20:14:21] [INFO] Started worker_1 (PID 1863): ./worker
[2026-11-25 20:14:21] [INFO] Started worker_2 (PID 1864): ./worker
[2026-11-25 20:14:21] [INFO] Started worker_3 (PID 1865): ./worker
[2026-11-25 20:14:21] [INFO] Started worker_4 (PID 1866): ./worker
[worker 1863] daemon mode started
[worker 1864] daemon mode started
[worker 1866] stubborn mode, ignoring SIGTERM
[worker 1865] normal mode, exiting with 0 in 10 s
[2026-11-25 20:14:22] [WARN] worker_1 (PID 1863) was killed by signal 9 (Killed)
[2026-11-25 20:14:22] [WARN] Restarting worker_1 (attempt 1/3)
[2026-11-25 20:14:22] [INFO] Restarted worker_1 with new PID 1875
[worker 1875] daemon mode started
[2026-11-25 20:14:23] [STATUS] === SIGUSR1 Process Status Report ===
[2026-11-25 20:14:23] [STATUS] Name: worker_1 | PID: 1875 | State: RUNNING | Restarts: 1/3
[2026-11-25 20:14:23] [STATUS] Name: worker_2 | PID: 1864 | State: RUNNING | Restarts: 0/3
[2026-11-25 20:14:23] [STATUS] Name: worker_3 | PID: 1865 | State: RUNNING | Restarts: 0/3
[2026-11-25 20:14:23] [STATUS] Name: worker_4 | PID: 1866 | State: RUNNING | Restarts: 0/3
[2026-11-25 20:14:23] [STATUS] Total: 4 | Running: 4
[2026-11-25 20:14:23] [STATUS] =====================================
^C[2026-11-25 20:14:24] [INFO] Shutdown mode on: no more restarts or new processes
[2026-11-25 20:14:24] [INFO] Graceful shutdown initiated
[2026-11-25 20:14:24] [INFO] Sent signal 15 (Terminated) to worker_1 (PID 1875)
[2026-11-25 20:14:24] [INFO] Sent signal 15 (Terminated) to worker_2 (PID 1864)
[2026-11-25 20:14:24] [INFO] Sent signal 15 (Terminated) to worker_3 (PID 1865)
[2026-11-25 20:14:24] [INFO] Sent signal 15 (Terminated) to worker_4 (PID 1866)
[2026-11-25 20:14:24] [WARN] worker_2 (PID 1864) was killed by signal 15 (Terminated)
[2026-11-25 20:14:24] [INFO] Not restarting worker_2: shutdown in progress
[2026-11-25 20:14:24] [WARN] worker_3 (PID 1865) was killed by signal 15 (Terminated)
[2026-11-25 20:14:24] [INFO] Not restarting worker_3: shutdown in progress
[2026-11-25 20:14:24] [WARN] worker_1 (PID 1875) was killed by signal 15 (Terminated)
[2026-11-25 20:14:24] [INFO] Not restarting worker_1: shutdown in progress
[2026-11-25 20:14:29] [WARN] Timeout reached (5 s), sending SIGKILL to remaining children
[2026-11-25 20:14:29] [INFO] Sent signal 9 (Killed) to worker_4 (PID 1866)
[2026-11-25 20:14:29] [WARN] worker_4 (PID 1866) was killed by signal 9 (Killed)
[2026-11-25 20:14:29] [INFO] Not restarting worker_4: shutdown in progress
[2026-11-25 20:14:29] [INFO] Shutdown complete, all children reaped
```
:::
*Figure 3: watchdog terminal during the same run: start-up, restart after
`kill -9`, SIGUSR1 report and Ctrl+C shutdown. The stubborn worker ignores
SIGTERM and is killed with SIGKILL after 5 seconds.*

::: term
```
$ pgrep -a -x worker; ps -eo pid,ppid,stat,args | awk '$3 ~ /^Z/ && /worker/'
# (no output: no worker process and no defunct entry is left)
```
:::
*Figure 4: after the shutdown no worker and no defunct process is left.*

## 5.3 Sample log: restart limit (scenario 4)

```
[2026-11-25 20:13:09] [INFO] Watchdog started (PID 1363)
[2026-11-25 20:13:09] [INFO] Started worker_1 (PID 1365): ./worker
[2026-11-25 20:13:09] [INFO] Started worker_2 (PID 1366): ./worker
[2026-11-25 20:13:09] [INFO] Started worker_3 (PID 1367): ./worker
[2026-11-25 20:13:09] [INFO] Started worker_4 (PID 1368): ./worker
[2026-11-25 20:13:11] [WARN] worker_1 (PID 1365) was killed by signal 9 (Killed)
[2026-11-25 20:13:11] [WARN] Restarting worker_1 (attempt 1/3)
[2026-11-25 20:13:11] [INFO] Restarted worker_1 with new PID 1381
[2026-11-25 20:13:13] [INFO] Health check: 4 running (PIDs: 1381, 1366, 1367, 1368)
[2026-11-25 20:13:13] [WARN] worker_1 (PID 1381) was killed by signal 9 (Killed)
[2026-11-25 20:13:13] [WARN] Restarting worker_1 (attempt 2/3)
[2026-11-25 20:13:13] [INFO] Restarted worker_1 with new PID 1398
[2026-11-25 20:13:15] [WARN] worker_1 (PID 1398) was killed by signal 9 (Killed)
[2026-11-25 20:13:15] [WARN] Restarting worker_1 (attempt 3/3)
[2026-11-25 20:13:15] [INFO] Restarted worker_1 with new PID 1415
[2026-11-25 20:13:17] [INFO] Health check: 4 running (PIDs: 1415, 1366, 1367, 1368)
[2026-11-25 20:13:17] [WARN] worker_1 (PID 1415) was killed by signal 9 (Killed)
[2026-11-25 20:13:17] [WARN] Restart limit reached for worker_1 (3/3): giving up
[2026-11-25 20:13:19] [INFO] worker_3 (PID 1367) exited normally with exit code 0
```

# 6. Commit history

Output of `git log --pretty=format:"%h %an %ad %s" --date=short` on `main`
after all modules and the final documentation were merged (26.11.2026).

```
(this commit) ZiyadAlboghdady 2026-11-26 docs: refresh commit history in report (#15)
3001995 ZiyadAlboghdady 2026-11-26 docs: correct Ziyad's student number in report (#15)
3c4b815 ZiyadAlboghdady 2026-11-26 docs: add student numbers and final commit history to report (#15)
1c09b52 AlperYildrm 2026-11-26 Merge pull request #22 from AlperYildrm/s3-final
b12b5f0 ZiyadAlboghdady 2026-11-25 docs: add project report (REPORT.md source and REPORT.pdf) (#15)
bf765fa ZiyadAlboghdady 2026-11-25 docs: document default workers, run directory and real status report in README (#15)
d9a9dba ZiyadAlboghdady 2026-11-25 test: add sample logs and output from the integrated test run (#14)
8068595 ZiyadAlboghdady 2026-11-25 fix(logger): close the log descriptor once instead of retrying on EINTR (#10)
9c11576 AlperYildrm 2026-11-25 Merge pull request #21 from AlperYildrm/implemetation-of-main.c
35e85ec AlperYildrm 2026-11-25 Implementing graceful shutdown and main control loop via main.c
3ecdcec Ammar Al Baghajati 2026-11-25 Merge pull request #20 from AlperYildrm/s2-ammar
4b6b338 ammarito 2026-11-25 feat(process): spawn, reaping, bounded restart and periodic check (#6, #7, #8, #9)
db45e20 ammarito 2026-11-25 feat(process): add process_manager.h interface (#5)
afbfc3a Ziyad Alboghdady 2026-11-24 Merge pull request #19 from AlperYildrm/s3-ziyad
6a260c2 ZiyadAlboghdady 2026-11-24 docs: write README with build, usage and signal commands (#15)
c57c42f ZiyadAlboghdady 2026-11-24 test: add automated test scenarios 1-7 (#14)
5610970 ZiyadAlboghdady 2026-11-24 feat(logger): timestamped O_APPEND logger and SIGUSR1 status report (#10, #11)
d0f7e5f ZiyadAlboghdady 2026-11-24 feat(logger): add logger interface (#10)
5e4b0ed ZiyadAlboghdady 2026-11-24 build: add Makefile for watchdog and worker with strict flags (#13)
ca922a7 ZiyadAlboghdady 2026-11-24 feat(worker): add daemon/normal/crash/stubborn worker modes (#12)
44d9427 AlperYildrm 2026-11-22 Merge pull request #18 from AlperYildrm/signal-manager-tasks
998b673 AlperYildrm 2026-11-22 rename: adjust the variable names according to issue
7d965b2 AlperYildrm 2026-11-22 Merge pull request #17 from AlperYildrm/signal-manager-tasks
2917c08 AlperYildrm 2026-11-22 refactor(signal): align signal_manager API and add signal masking helpers
ff3e59c AlperYildrm 2026-11-21 added signal_manager module with async-signal-safe handlers
5b2f654 AlperYildrm 2026-11-21 Project structure is created
```

# 7. Conclusion

The watchdog meets every requirement of the assignment: handlers only set
flags, the main loop waits with `sigsuspend()` without busy waiting, children are
created with `fork()`/`execv()` and reaped with a `waitpid()` loop, failed
children are restarted at most three times, SIGUSR1 produces a status report,
and SIGTERM/SIGINT shut everything down with a SIGKILL fallback and no zombie
processes. All seven test scenarios pass on the integrated code.
