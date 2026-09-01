/* log.h - one line per event, on stderr, for both the shell and the journal. */
#ifndef DE_LOG_H
#define DE_LOG_H

void log_info(const char *fmt, ...);
void log_err(const char *fmt, ...);

/* Enabled by -v; off by default. */
void log_set_verbose(int on);
void log_debug(const char *fmt, ...);

#endif /* DE_LOG_H */
