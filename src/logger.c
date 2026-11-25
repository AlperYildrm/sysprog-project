#define _POSIX_C_SOURCE 200809L

#include "logger.h"
#include "process_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX 1024

static int g_log_fd = -1;

/* write() the whole buffer, retrying on partial writes and EINTR. */
static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

/* Fill buf with "YYYY-MM-DD HH:MM:SS" in local time. */
static void format_timestamp(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    if (localtime_r(&now, &tm_now) == NULL ||
        strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm_now) == 0)
        snprintf(buf, size, "0000-00-00 00:00:00");
}

int init_logger(const char *log_file_path)
{
    if (log_file_path == NULL || *log_file_path == '\0') {
        fprintf(stderr, "init_logger: empty log file path\n");
        return -1;
    }
    if (g_log_fd >= 0)
        close_logger();

    g_log_fd = open(log_file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_log_fd < 0) {
        fprintf(stderr, "init_logger: cannot open '%s': %s\n",
                log_file_path, strerror(errno));
        return -1;
    }
    /* Children run exec'd workers: they must not inherit the log fd. */
    (void)fcntl(g_log_fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

void log_event(const char *level, const char *format, ...)
{
    char line[LOG_LINE_MAX];
    char ts[32];
    va_list ap;
    int len, body;

    format_timestamp(ts, sizeof(ts));
    len = snprintf(line, sizeof(line), "[%s] [%s] ", ts,
                   (level != NULL && *level != '\0') ? level : "INFO");
    if (len < 0)
        return;
    if ((size_t)len >= sizeof(line))
        len = (int)sizeof(line) - 1;

    va_start(ap, format);
    body = vsnprintf(line + len, sizeof(line) - (size_t)len, format, ap);
    va_end(ap);
    if (body < 0)
        body = 0;
    len += body;
    if ((size_t)len > sizeof(line) - 2) /* truncated: keep room for '\n' */
        len = (int)sizeof(line) - 2;

    /* Strip a trailing newline the caller may have added, then add one. */
    while (len > 0 && line[len - 1] == '\n')
        len--;
    line[len++] = '\n';
    line[len] = '\0';

    if (g_log_fd >= 0) {
        write_all(g_log_fd, line, (size_t)len);
        write_all(STDOUT_FILENO, line, (size_t)len);
    } else {
        write_all(STDERR_FILENO, line, (size_t)len);
    }
}

void log_status_report(void)
{
    int count = 0;
    int running = 0;
    int i;
    const ChildProcess *children = get_process_list(&count);

    log_event("STATUS", "=== SIGUSR1 Process Status Report ===");
    if (children == NULL || count <= 0) {
        log_event("STATUS", "No managed child processes");
    } else {
        for (i = 0; i < count; i++) {
            const ChildProcess *c = &children[i];
            if (c->state == PROC_RUNNING)
                running++;
            log_event("STATUS", "Name: %s | PID: %ld | State: %s | Restarts: %d/%d",
                      c->name, (long)c->pid, process_state_to_string(c->state),
                      c->restart_count, MAX_RESTARTS);
        }
        log_event("STATUS", "Total: %d | Running: %d", count, running);
    }
    log_event("STATUS", "=====================================");
}

void close_logger(void)
{
    /* close() is not retried on EINTR: on Linux the descriptor is already
     * released at that point, and a retry could close an unrelated fd. */
    if (g_log_fd >= 0) {
        (void)close(g_log_fd);
        g_log_fd = -1;
    }
}
