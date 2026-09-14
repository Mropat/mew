// Shared logging. Writes to Mewgenics/mod_logs/<MODLOG_NAME>.log, next to
// mewjector's own chainloader.log, rather than dropping files beside the game
// exe where they look like stray junk to anyone who did not put them there.
//
// Define MODLOG_NAME before including.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#ifndef MODLOG_NAME
#error "define MODLOG_NAME before including modlog.inc"
#endif

static void logf_(const char* fmt, ...) {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH)) return;
    char* slash = strrchr(path, (char)92);              // backslash
    if (!slash) return;

    lstrcpyA(slash + 1, "mod_logs");
    CreateDirectoryA(path, NULL);                       // harmless if it exists
    lstrcatA(path, "\\" MODLOG_NAME ".log");

    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
