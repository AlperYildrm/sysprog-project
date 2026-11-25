#define _POSIX_C_SOURCE 200809L

#include "process_manager.h"

#include "logger.h"
#include "signal_manager.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_CHECK_INTERVAL_SEC 4 /* default period of the SIGALRM check */

/* ---- module state (the child table) ------------------------------------ */

static ChildProcess g_children[MAX_CHILDREN];
static int g_child_count = 0;
static int g_shutdown_mode = 0;

/* ---- helpers ----------------------------------------------------------- */

static ChildProcess *find_by_pid(pid_t pid)
{
    int i;

    for (i = 0; i < g_child_count; i++) {
        if (g_children[i].pid == pid && g_children[i].state == PROC_RUNNING)
            return &g_children[i];
    }
    return NULL;
}

static int spawn(ChildProcess *c)
{
    char *argv[MAX_ARGS + 1];
    pid_t pid;
    int i;

    /* Build the argv array for execv() from the copied strings. */
    for (i = 0; i < c->arg_count; i++)
        argv[i] = c->args[i];
    argv[c->arg_count] = NULL;

    pid = fork();
    if (pid < 0) {
        log_event("ERROR", "fork() failed for %s: %s", c->name, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        struct sigaction dfl;
        sigset_t none;
        static const int reset_signals[] = {SIGINT, SIGTERM, SIGUSR1, SIGCHLD, SIGALRM};
        size_t k;

        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        for (k = 0; k < sizeof(reset_signals) / sizeof(reset_signals[0]); k++)
            sigaction(reset_signals[k], &dfl, NULL);

        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);

        execv(c->executable, argv);

        perror("execv");
        _exit(EXIT_FAILURE);
    }

    /* PARENT */
    c->pid = pid;
    c->state = PROC_RUNNING;
    c->last_start_time = time(NULL);
    return 0;
}

static void restart_or_give_up(ChildProcess *c)
{
    if (g_shutdown_mode || g_shutdown_requested) {
        log_event("INFO", "Not restarting %s: shutdown in progress", c->name);
        return;
    }

    if (c->restart_count >= MAX_RESTARTS) {
        c->state = PROC_RESTART_LIMIT_REACHED;
        c->pid = -1;
        log_event("WARN", "Restart limit reached for %s (%d/%d): giving up",
                  c->name, c->restart_count, MAX_RESTARTS);
        return;
    }

    c->restart_count++;
    log_event("WARN", "Restarting %s (attempt %d/%d)", c->name,
              c->restart_count, MAX_RESTARTS);

    if (spawn(c) == 0) {
        log_event("INFO", "Restarted %s with new PID %ld", c->name, (long)c->pid);
    } else {
        c->state = PROC_TERMINATED_ERROR;
        log_event("ERROR", "Could not restart %s", c->name);
    }
}

/* ---- public API -------------------------------------------------------- */

int init_process_manager(int interval_seconds)
{
    struct sigaction current;
    struct itimerval timer;

    if (interval_seconds <= 0)
        interval_seconds = DEFAULT_CHECK_INTERVAL_SEC;

    memset(g_children, 0, sizeof(g_children));
    g_child_count = 0;
    g_shutdown_mode = 0;

    if (sigaction(SIGALRM, NULL, &current) == -1 ||
        current.sa_handler == SIG_DFL || current.sa_handler == SIG_IGN) {
        log_event("ERROR", "SIGALRM handler not installed; "
                           "call init_signal_handlers() before init_process_manager()");
        return -1;
    }

    timer.it_value.tv_sec = interval_seconds;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = interval_seconds;
    timer.it_interval.tv_usec = 0;
    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
        log_event("ERROR", "setitimer() failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int add_and_start_process(const char *name, const char *path, char *const argv[])
{
    ChildProcess *c;
    int i;

    if (name == NULL || path == NULL || argv == NULL || argv[0] == NULL) {
        log_event("ERROR", "add_and_start_process: invalid arguments");
        return -1;
    }
    if (g_shutdown_mode || g_shutdown_requested) {
        log_event("WARN", "Not starting %s: shutdown in progress", name);
        return -1;
    }
    if (g_child_count >= MAX_CHILDREN) {
        log_event("ERROR", "Cannot start %s: child table is full (%d)", name,
                  MAX_CHILDREN);
        return -1;
    }

    c = &g_children[g_child_count];
    memset(c, 0, sizeof(*c));

    if (snprintf(c->name, sizeof(c->name), "%s", name) >= (int)sizeof(c->name) ||
        snprintf(c->executable, sizeof(c->executable), "%s", path) >=
            (int)sizeof(c->executable)) {
        log_event("ERROR", "Name or path too long for %s", name);
        return -1;
    }
    for (i = 0; argv[i] != NULL; i++) {
        if (i >= MAX_ARGS ||
            snprintf(c->args[i], sizeof(c->args[i]), "%s", argv[i]) >=
                (int)sizeof(c->args[i])) {
            log_event("ERROR", "Too many or too long arguments for %s", name);
            return -1;
        }
    }
    c->arg_count = i;
    c->restart_count = 0;

    if (spawn(c) == -1)
        return -1;

    g_child_count++;
    log_event("INFO", "Started %s (PID %ld): %s", c->name, (long)c->pid,
              c->executable);
    return 0;
}

void handle_child_terminations(void)
{
    int status;
    pid_t pid;
    ChildProcess *c;

    g_child_terminated = 0;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        c = find_by_pid(pid);
        if (c == NULL) {
            log_event("WARN", "Reaped unknown child PID %ld", (long)pid);
            continue;
        }

        c->pid = -1;

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);

            if (code == 0) {
                c->state = PROC_EXITED_NORMAL;
                log_event("INFO", "%s (PID %ld) exited normally with exit code 0",
                          c->name, (long)pid);
            } else {
                c->state = PROC_TERMINATED_ERROR;
                log_event("WARN", "%s (PID %ld) exited with error, exit code %d",
                          c->name, (long)pid, code);
                restart_or_give_up(c);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);

            c->state = PROC_TERMINATED_ERROR;
            log_event("WARN", "%s (PID %ld) was killed by signal %d (%s)", c->name,
                      (long)pid, sig, strsignal(sig));
            restart_or_give_up(c);
        }
    }

    if (pid == -1 && errno != ECHILD && errno != EINTR)
        log_event("ERROR", "waitpid() failed: %s", strerror(errno));
}

void periodic_process_check(void)
{
    char pids[160];
    size_t used = 0;
    int i, running = 0;

    g_alarm_triggered = 0;

    /* Safety net for a missed SIGCHLD. */
    handle_child_terminations();

    for (i = 0; i < g_child_count; i++) {
        ChildProcess *c = &g_children[i];

        if (c->state != PROC_RUNNING)
            continue;

        /* kill(pid, 0) sends no signal; it only tells whether the PID exists. */
        if (kill(c->pid, 0) == -1 && errno == ESRCH) {
            log_event("WARN", "%s (PID %ld) no longer exists", c->name,
                      (long)c->pid);
            c->state = PROC_TERMINATED_ERROR;
            c->pid = -1;
            restart_or_give_up(c);
            if (c->state != PROC_RUNNING)
                continue;
        }

        running++;
        if (used < sizeof(pids)) {
            int n = snprintf(pids + used, sizeof(pids) - used, "%s%ld",
                             used ? ", " : "", (long)c->pid);
            if (n > 0 && (size_t)n < sizeof(pids) - used)
                used += (size_t)n;
        }
    }

    if (running > 0)
        log_event("INFO", "Health check: %d running (PIDs: %s)", running, pids);
}

void set_shutdown_mode(int enabled)
{
    g_shutdown_mode = enabled ? 1 : 0;
    if (g_shutdown_mode)
        log_event("INFO", "Shutdown mode on: no more restarts or new processes");
}

void send_signal_to_all_children(int sig)
{
    int i;

    for (i = 0; i < g_child_count; i++) {
        ChildProcess *c = &g_children[i];

        if (c->state != PROC_RUNNING)
            continue;
        if (kill(c->pid, sig) == 0)
            log_event("INFO", "Sent signal %d (%s) to %s (PID %ld)", sig,
                      strsignal(sig), c->name, (long)c->pid);
        else if (errno != ESRCH) /* ESRCH: already gone, nothing to do */
            log_event("ERROR", "kill(%ld, %d) failed: %s", (long)c->pid, sig,
                      strerror(errno));
    }
}

int count_running_children(void)
{
    int i, n = 0;

    for (i = 0; i < g_child_count; i++) {
        if (g_children[i].state == PROC_RUNNING)
            n++;
    }
    return n;
}

const ChildProcess *get_process_list(int *count)
{
    if (count != NULL)
        *count = g_child_count;
    return g_children;
}

const char *process_state_to_string(ProcessState state)
{
    switch (state) {
    case PROC_RUNNING:                return "RUNNING";
    case PROC_EXITED_NORMAL:          return "EXITED_NORMAL";
    case PROC_TERMINATED_ERROR:       return "TERMINATED_ERROR";
    case PROC_RESTART_LIMIT_REACHED:  return "RESTART_LIMIT_REACHED";
    }
    return "UNKNOWN";
}
