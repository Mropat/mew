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

    // Truncate on the first line of each run. A mod that says one thing per
    // launch would otherwise leave a file that only ever grows, and the last
    // run - the one anyone would be asking about - would be buried.
    static bool started = false;
    FILE* f = fopen(path, started ? "a" : "w");
    if (!f) return;
    started = true;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
