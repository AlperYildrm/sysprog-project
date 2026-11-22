#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>

extern volatile sig_atomic_t g_shutdown_requested;
extern volatile sig_atomic_t g_status_requested;
extern volatile sig_atomic_t g_child_terminated;
extern volatile sig_atomic_t g_alarm_triggered;

int init_signal_handlers(void);
int block_managed_signals(sigset_t *old_mask);
int restore_signal_mask(const sigset_t *old_mask);

#endif