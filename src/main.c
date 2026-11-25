#define _POSIX_C_SOURCE 200809L

#include "logger.h"
#include "process_manager.h"
#include "signal_manager.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHUTDOWN_TIMEOUT_SEC 5
#define LOG_FILE             "watchdog.log"

int main(void)
{
    sigset_t old_mask;

    if (init_logger(LOG_FILE) == -1) {
        fprintf(stderr, "Failed to initialize logger\n");
        return 1;
    }

    if (init_signal_handlers() == -1) {
        log_event("ERROR", "Failed to initialize signal handlers");
        close_logger();
        return 1;
    }

    if (init_process_manager(4) == -1) {
        log_event("ERROR", "Failed to initialize process manager");
        close_logger();
        return 1;
    }

    log_event("INFO", "Watchdog started (PID %ld)", (long)getpid());

    char *args_daemon1[]  = {"./worker", "daemon", NULL};
    char *args_daemon2[]  = {"./worker", "daemon", NULL};
    char *args_normal[]   = {"./worker", "normal", "10", NULL};
    char *args_stubborn[] = {"./worker", "stubborn", NULL};

    if (add_and_start_process("worker_1", "./worker", args_daemon1) == -1)
        log_event("ERROR", "Failed to start worker_1");
    if (add_and_start_process("worker_2", "./worker", args_daemon2) == -1)
        log_event("ERROR", "Failed to start worker_2");
    if (add_and_start_process("worker_3", "./worker", args_normal) == -1)
        log_event("ERROR", "Failed to start worker_3");
    if (add_and_start_process("worker_4", "./worker", args_stubborn) == -1)
        log_event("ERROR", "Failed to start worker_4");

    if (block_managed_signals(&old_mask) == -1) {
        log_event("ERROR", "Failed to block signals");
        close_logger();
        return 1;
    }

    while (!g_shutdown_requested) {

        if (g_child_terminated) {
            g_child_terminated = 0;
            handle_child_terminations();
        }

        if (g_alarm_triggered) {
            g_alarm_triggered = 0;
            periodic_process_check();
        }

        if (g_status_requested) {
            g_status_requested = 0;
            log_status_report();
        }

        sigsuspend(&old_mask);
    }

    set_shutdown_mode(1);

    log_event("INFO", "Graceful shutdown initiated");

    send_signal_to_all_children(SIGTERM);

    {
        time_t deadline = time(NULL) + SHUTDOWN_TIMEOUT_SEC;

        while (count_running_children() > 0 && time(NULL) < deadline) {
            handle_child_terminations();

            if (count_running_children() > 0) {
                struct timespec ts = {0, 100000000L};
                nanosleep(&ts, NULL);
            }
        }
    }

    if (count_running_children() > 0) {
        log_event("WARN",
                  "Timeout reached (%d s), sending SIGKILL to remaining children",
                  SHUTDOWN_TIMEOUT_SEC);

        send_signal_to_all_children(SIGKILL);

        {
            struct timespec ts = {0, 200000000L};
            nanosleep(&ts, NULL);
        }

        handle_child_terminations();
    }

    {
        int st;
        while (waitpid(-1, &st, WNOHANG) > 0)
            ;
    }

    log_event("INFO", "Shutdown complete, all children reaped");
    close_logger();

    restore_signal_mask(&old_mask);

    return 0;
}
