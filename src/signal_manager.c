#define _POSIX_C_SOURCE 200809L

#include "signal_manager.h"
#include <errno.h>
#include <stddef.h>

volatile sig_atomic_t g_sigchld_received = 0;
volatile sig_atomic_t g_sigusr1_received = 0;
volatile sig_atomic_t g_sigalrm_received = 0;
volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_sigchld_received = 1;
    errno = saved_errno;
}

static void handle_sigusr1(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_sigusr1_received = 1;
    errno = saved_errno;
}

static void handle_sigalrm(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_sigalrm_received = 1;
    errno = saved_errno;
}

static void handle_shutdown(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_shutdown_requested = 1;
    errno = saved_errno;
}

int setup_signal_handlers(void) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sa.sa_handler = handle_sigchld;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        return -1;
    }

    sa.sa_handler = handle_sigusr1;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        return -1;
    }

    sa.sa_handler = handle_sigalrm;
    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        return -1;
    }

    sa.sa_handler = handle_shutdown;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return -1;
    }

    return 0;
}