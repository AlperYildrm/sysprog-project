#ifndef LOGGER_H
#define LOGGER_H

/*
 * logger - timestamped event log for the Watchdog.
 *
 * Every line has the form:
 *   [YYYY-MM-DD HH:MM:SS] [LEVEL] message
 *
 * Suggested levels: INFO, WARN, ERROR, STATUS.
 * Lines are written with a single write() on a file opened with O_APPEND,
 * so they are never interleaved and never sit in a stdio buffer.
 * Lines are also echoed to stdout so the watchdog can be watched live.
 *
 * None of these functions are async-signal-safe: call them from the main
 * loop only, never from a signal handler.
 */

/* Open (create/append) the log file. Returns 0 on success, -1 on error. */
int init_logger(const char *log_file_path);

/* printf-style log line. Falls back to stderr if the logger is not open. */
void log_event(const char *level, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* SIGUSR1 report: one line per managed child (needs process_manager). */
void log_status_report(void);

/* Close the log file. Safe to call more than once. */
void close_logger(void);

#endif /* LOGGER_H */
