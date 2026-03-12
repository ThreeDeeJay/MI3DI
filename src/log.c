/*
 * MI3DI – Logging
 *
 * Verbose logging is enabled when the environment variable MI3DI_LOG
 * is set to a file path.  Timestamps are intentionally omitted.
 */
#include "log.h"
#include "version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static FILE  *g_logfile  = NULL;
static CRITICAL_SECTION g_cs;
static int    g_ready    = 0;

void log_init(void)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("MI3DI_LOG", path, sizeof(path)))
        return;

    InitializeCriticalSection(&g_cs);

    g_logfile = fopen(path, "a");
    if (!g_logfile) return;

    g_ready = 1;
    log_write("=== " MI3DI_FULL_VERSION " – logging started ===");
}

void log_close(void)
{
    if (!g_ready) return;
    log_write("=== " MI3DI_FULL_VERSION " – logging stopped ===");
    fclose(g_logfile);
    g_logfile = NULL;
    g_ready   = 0;
    DeleteCriticalSection(&g_cs);
}

void log_write(const char *fmt, ...)
{
    if (!g_ready || !g_logfile) return;

    va_list ap;
    va_start(ap, fmt);

    EnterCriticalSection(&g_cs);
    vfprintf(g_logfile, fmt, ap);
    fputc('\n', g_logfile);
    fflush(g_logfile);
    LeaveCriticalSection(&g_cs);

    va_end(ap);
}

int log_enabled(void)
{
    return g_ready;
}
