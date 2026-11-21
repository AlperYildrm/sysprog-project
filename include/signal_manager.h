#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

#include <signal.h>

extern volatile sig_atomic_t g_sigchld_received;
extern volatile sig_atomic_t g_sigusr1_received;
extern volatile sig_atomic_t g_sigalrm_received;
extern volatile sig_atomic_t g_shutdown_requested;

int setup_signal_handlers(void);

#endif /* SIGNAL_MANAGER_H */