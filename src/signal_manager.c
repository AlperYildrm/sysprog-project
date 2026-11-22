#define _POSIX_C_SOURCE 200809L

#include "signal_manager.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

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

static void build_managed_signal_set(sigset_t *set) {
    sigemptyset(set);
    sigaddset(set, SIGCHLD);
    sigaddset(set, SIGUSR1);
    sigaddset(set, SIGALRM);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
}

int setup_signal_handlers(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sa.sa_handler = handle_sigchld;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction(SIGCHLD)");
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sa.sa_handler = handle_sigusr1;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction(SIGUSR1)");
        return -1;
    }

    sa.sa_handler = handle_sigalrm;
    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        perror("sigaction(SIGALRM)");
        return -1;
    }

    sa.sa_handler = handle_shutdown;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction(SIGINT)");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction(SIGTERM)");
        return -1;
    }

    return 0;
}

int block_managed_signals(sigset_t *old_mask) {
    sigset_t block_set;
    build_managed_signal_set(&block_set);

    if (sigprocmask(SIG_BLOCK, &block_set, old_mask) == -1) {
        perror("sigprocmask(SIG_BLOCK)");
        return -1;
    }
    return 0;
}

int restore_signal_mask(const sigset_t *old_mask) {
    if (old_mask != NULL) {
        if (sigprocmask(SIG_SETMASK, old_mask, NULL) == -1) {
            perror("sigprocmask(SIG_SETMASK)");
            return -1;
        }
    } else {
        sigset_t unblock_set;
        build_managed_signal_set(&unblock_set);
        if (sigprocmask(SIG_UNBLOCK, &unblock_set, NULL) == -1) {
            perror("sigprocmask(SIG_UNBLOCK)");
            return -1;
        }
    }
    return 0;
}