/*
 * worker.c - test child process for the Watchdog project.
 *
 * Usage:
 *   worker daemon              run forever (pause loop); default SIGTERM action ends it
 *   worker normal <sec>        run <sec> seconds, then exit(0)
 *   worker crash <sec> [abort] run <sec> seconds, then exit(1) (or abort() -> SIGABRT)
 *   worker stubborn            run forever and IGNORE SIGTERM (needs SIGKILL)
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <mode> [args]\n"
            "Modes:\n"
            "  daemon              run until killed (SIGTERM/SIGINT/SIGKILL)\n"
            "  normal <sec>        run <sec> seconds, then exit with code 0\n"
            "  crash <sec> [abort] run <sec> seconds, then exit with code 1\n"
            "                      (or die by SIGABRT when 'abort' is given)\n"
            "  stubborn            run forever and ignore SIGTERM\n",
            prog);
}

/* Parse a positive number of seconds; returns -1 on bad input. */
static long parse_seconds(const char *s)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v <= 0 || v > 86400)
        return -1;
    return v;
}

/* Sleep for sec seconds, resuming after interrupted calls. */
static void sleep_seconds(long sec)
{
    struct timespec req, rem;

    req.tv_sec = (time_t)sec;
    req.tv_nsec = 0;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
}

static int install(int sig, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    return sigaction(sig, &sa, NULL);
}

int main(int argc, char *argv[])
{
    const char *mode;
    long sec = 0;
    pid_t pid = getpid();

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    mode = argv[1];

    if (strcmp(mode, "daemon") == 0 && argc == 2) {
        /* Default SIGTERM/SIGINT action: the watchdog sees "killed by signal". */
        printf("[worker %ld] daemon mode started\n", (long)pid);
        fflush(stdout);
        for (;;)
            pause();
    }

    if (strcmp(mode, "normal") == 0 && argc == 3) {
        if ((sec = parse_seconds(argv[2])) < 0) {
            usage(argv[0]);
            return 2;
        }
        printf("[worker %ld] normal mode, exiting with 0 in %ld s\n", (long)pid, sec);
        fflush(stdout);
        sleep_seconds(sec);
        return 0;
    }

    if (strcmp(mode, "crash") == 0 && (argc == 3 || argc == 4)) {
        int use_abort = 0;

        if ((sec = parse_seconds(argv[2])) < 0) {
            usage(argv[0]);
            return 2;
        }
        if (argc == 4) {
            if (strcmp(argv[3], "abort") != 0) {
                usage(argv[0]);
                return 2;
            }
            use_abort = 1;
        }
        printf("[worker %ld] crash mode, failing in %ld s (%s)\n", (long)pid, sec,
               use_abort ? "SIGABRT" : "exit code 1");
        fflush(stdout);
        sleep_seconds(sec);
        if (use_abort)
            abort();
        return 1;
    }

    if (strcmp(mode, "stubborn") == 0 && argc == 2) {
        install(SIGTERM, SIG_IGN);
        printf("[worker %ld] stubborn mode, ignoring SIGTERM\n", (long)pid);
        fflush(stdout);
        for (;;)
            pause();
    }

    usage(argv[0]);
    return 2;
}
