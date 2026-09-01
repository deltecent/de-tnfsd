#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int verbose;

void log_set_verbose(int on)
{
    verbose = on;
}

static void emit(const char *level, const char *fmt, va_list ap)
{
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;

    if (gmtime_r(&now, &tm) != NULL)
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm);
    else
        stamp[0] = '\0';

    fprintf(stderr, "%s %s ", stamp, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("info", fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("error", fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...)
{
    va_list ap;
    if (!verbose)
        return;
    va_start(ap, fmt);
    emit("debug", fmt, ap);
    va_end(ap);
}
