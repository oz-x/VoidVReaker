#include "log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace vvr {

namespace {
FILE* g_file = nullptr;
std::mutex g_mutex;
}  // namespace

void log_init(const wchar_t* path) {
    std::lock_guard lock(g_mutex);
    if (g_file == nullptr) {
        g_file = _wfsopen(path, L"w", _SH_DENYWR);
    }
}

void logf(const char* fmt, ...) {
    char buf[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d][%5lu] ", t.wHour, t.wMinute,
                     t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, args);
    va_end(args);

    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
        fputs(buf, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
}

}  // namespace vvr
