# Watchdog — POSIX signal-based process monitor

`watchdog` is a small supervisor written in C for Linux/POSIX systems. It starts a
set of child processes, watches them with signals, restarts the ones that fail
(up to 3 times each), prints a status report on demand and shuts everything down
cleanly without leaving zombie processes.

Term Project 1 — BSM308 / SWE-301 System Programming, Sakarya University.

| Student | GitHub | Modules |
|---|---|---|
| S1 Alper | [AlperYildrm](https://github.com/AlperYildrm) | `signal_manager.h/.c`, `main.c` |
| S2 Ammar | [Ammarito9](https://github.com/Ammarito9) | `process_manager.h/.c` |
| S3 Ziyad | [ziyad-alboghdady](https://github.com/ziyad-alboghdady) | `logger.h/.c`, `workers/worker.c`, `Makefile`, tests, docs |

## How it works

* **Signal handlers only set flags** (`volatile sig_atomic_t`). All real work is done
  in the main loop, which blocks the managed signals and waits with `sigsuspend()`,
  so no signal can be lost between checking a flag and going to sleep.
* **SIGCHLD** → `handle_child_terminations()` reaps every finished child with
  `waitpid(-1, &status, WNOHANG)` and decides whether to restart it.
* **SIGALRM** → `periodic_process_check()` runs a periodic health check.
* **SIGUSR1** → `log_status_report()` writes the state of every child to the log.
* **SIGTERM / SIGINT (Ctrl+C)** → graceful shutdown: SIGTERM to all children, wait
  a few seconds, SIGKILL whatever is left, reap everything, close the log.

Restart policy: a child that exits with code 0 has finished its job and is not
restarted. A child that exits with a non-zero code or is killed by a signal is
restarted while its restart count is below 3; after that it is marked
`RESTART_LIMIT_REACHED` and left stopped.

## Project layout

```
.
├── Makefile
├── README.md
├── include/
│   ├── signal_manager.h    signal flags, handler setup, signal masking
│   ├── process_manager.h   child table, start/restart/reap, shutdown helpers
│   └── logger.h            timestamped log + SIGUSR1 status report
├── src/
│   ├── main.c              start-up, main event loop, graceful shutdown
│   ├── signal_manager.c
│   ├── process_manager.c
│   └── logger.c
├── workers/
│   └── worker.c            configurable test child (daemon/normal/crash/stubborn)
├── tests/
│   ├── test_scenarios.sh   automated test scenarios
│   └── sample_logs/        logs saved by the last test run
└── docs/
    ├── REPORT.md           report source
    └── REPORT.pdf          project report
```

## Building

Requires `gcc` and `make` on Linux (or WSL).

```sh
make          # builds ./watchdog and ./worker
make run      # builds and runs ./watchdog in the foreground
make test     # builds and runs tests/test_scenarios.sh
make clean    # removes objects, binaries and watchdog.log
```

Compiler flags: `-Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L`.
The build produces no warnings.

## Usage

```sh
./watchdog
```

Run it from the project root: the workers are started as `./worker`, so the
`worker` binary must be in the current directory (`make run` does this for you).

On start-up the watchdog launches four workers (set in `src/main.c`):

| Name | Command | What it shows |
|---|---|---|
| `worker_1` | `./worker daemon` | long-running child, restarted if it is killed |
| `worker_2` | `./worker daemon` | second long-running child |
| `worker_3` | `./worker normal 10` | exits with code 0 after 10 s and is not restarted |
| `worker_4` | `./worker stubborn` | ignores SIGTERM, so shutdown falls back to SIGKILL |

Every 4 seconds a SIGALRM health check (`setitimer`) logs the running PIDs and
catches any child whose SIGCHLD was missed. On shutdown the watchdog waits up to
5 seconds after SIGTERM before sending SIGKILL.

The watchdog writes its log to `watchdog.log` in the current directory and also
echoes each line to the terminal. Every line has the form

```
[YYYY-MM-DD HH:MM:SS] [LEVEL] message
```

### The worker program

`worker` is the child program the watchdog supervises. Its first argument picks
how it behaves, which makes every scenario easy to reproduce:

| Command | Behaviour |
|---|---|
| `./worker daemon` | runs forever (`pause()` loop); ends on SIGTERM, SIGINT or SIGKILL |
| `./worker normal <sec>` | runs `<sec>` seconds, then exits with code 0 (not restarted) |
| `./worker crash <sec>` | runs `<sec>` seconds, then exits with code 1 (restarted) |
| `./worker crash <sec> abort` | runs `<sec>` seconds, then dies by SIGABRT (restarted) |
| `./worker stubborn` | runs forever and **ignores SIGTERM**, so shutdown must use SIGKILL |

Wrong arguments print a usage message and exit with code 2.

## Signal commands

Open a second terminal and find the watchdog PID with `pgrep -x watchdog`.

| Command | Effect |
|---|---|
| `kill -USR1 $(pgrep -x watchdog)` | write a process status report to `watchdog.log` |
| `kill -TERM $(pgrep -x watchdog)` | graceful shutdown |
| `Ctrl+C` in the watchdog terminal (SIGINT) | graceful shutdown |
| `kill -9 <worker PID>` | simulate a crash: the worker is restarted (max 3 times) |
| `ps -o pid,ppid,stat,args --ppid $(pgrep -x watchdog)` | list the watchdog's children |

Example status report (SIGUSR1):

```
[2026-11-25 20:13:26] [STATUS] === SIGUSR1 Process Status Report ===
[2026-11-25 20:13:26] [STATUS] Name: worker_1 | PID: 1502 | State: RUNNING | Restarts: 0/3
[2026-11-25 20:13:26] [STATUS] Name: worker_2 | PID: 1503 | State: RUNNING | Restarts: 0/3
[2026-11-25 20:13:26] [STATUS] Name: worker_3 | PID: 1504 | State: RUNNING | Restarts: 0/3
[2026-11-25 20:13:26] [STATUS] Name: worker_4 | PID: 1505 | State: RUNNING | Restarts: 0/3
[2026-11-25 20:13:26] [STATUS] Total: 4 | Running: 4
[2026-11-25 20:13:26] [STATUS] =====================================
```

Possible states: `RUNNING`, `EXITED_NORMAL`, `TERMINATED_ERROR`,
`RESTART_LIMIT_REACHED` (a child that is not running shows PID -1).

## Tests

`tests/test_scenarios.sh` builds the project and runs each scenario against a fresh
watchdog, then prints PASS/FAIL per check and saves the log of every scenario in
`tests/sample_logs/`.

1. At least two workers start and their PIDs appear in the log.
2. A worker that exits with code 0 is not restarted.
3. A worker killed with SIGKILL is restarted.
4. Killing the same worker four times gives exactly three restarts, then the
   restart limit is logged and the worker stays stopped.
5. SIGUSR1 writes the status report to `watchdog.log`.
6. SIGTERM and SIGINT (Ctrl+C) shut down every worker with no defunct processes.
7. Every log line starts with `[YYYY-MM-DD HH:MM:SS] [LEVEL]`.

Environment variables `WATCHDOG`, `WD_ARGS`, `LOG` and `STARTUP` override the
defaults (see the top of the script).

The whole run takes about a minute and ends with a summary line. The output and
logs of the last run on the integrated code (19 passed, 0 failed) are in
`tests/sample_logs/`, including `test_run_output.txt`.
