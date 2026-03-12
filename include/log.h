#pragma once
#include <stdarg.h>

void log_init(void);
void log_close(void);
void log_write(const char *fmt, ...);
int  log_enabled(void);

/* Convenience macro – emits nothing when logging is off */
#define LOG(...) do { if (log_enabled()) log_write(__VA_ARGS__); } while (0)
