#pragma once
#include <cstdarg>

namespace vvr {

void log_init(const wchar_t* path);
void logf(const char* fmt, ...);

}  // namespace vvr

#define LOG(...) ::vvr::logf(__VA_ARGS__)
