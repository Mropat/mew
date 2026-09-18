// Tracing, for working on this mod - not for shipping.
//
// A released mod has no business writing a log file: the three things a user
// could care about (installed / refused, and why) go through MJ_Log to
// mod_logs/chainloader.log, alongside every other mod's.
//
// Everything else here exists to answer questions about alignment and about the
// settings row, and those questions took a while to answer, so it is kept
// rather than deleted. Build with -DLEGENDOFBUNGA_TRACE and it writes
// mod_logs/legendofbunga.log: what was queued, where each stem is, when the
// loop hands over, what every cat's turn decided, and how the row was built.
//
// Without it say() is a macro that expands to nothing, so the calls and the
// text they would have printed are both gone. An empty function would not have
// done: its arguments still get compiled in, and the whole log was legible in
// the shipped binary.

#ifdef LEGENDOFBUNGA_TRACE

static FILE* g_log;

static void trace_open(void) {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH)) return;
    char* slash = strrchr(path, (char)92);
    if (!slash) return;
    lstrcpyA(slash + 1, "mod_logs");
    CreateDirectoryA(path, NULL);
    lstrcatA(path, "\\legendofbunga.log");
    g_log = fopen(path, "w");
}

static void say_(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); va_end(ap);
    fputc(10, g_log); fflush(g_log);
}

#define say(...) say_(__VA_ARGS__)

#else   // shipping: no log file, no formatting, nothing left in the binary

static void trace_open(void) {}
#define say(...) ((void)0)

#endif
