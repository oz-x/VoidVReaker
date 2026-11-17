// version.dll proxy: every export jumps straight into the system version.dll (stubs in
// proxy_stubs.asm read the g_real_* pointers resolved here).
#include <windows.h>

#include "proxy.h"

#define VVR_EXPORTS(X)               \
    X(GetFileVersionInfoA)           \
    X(GetFileVersionInfoByHandle)    \
    X(GetFileVersionInfoExA)         \
    X(GetFileVersionInfoExW)         \
    X(GetFileVersionInfoSizeA)       \
    X(GetFileVersionInfoSizeExA)     \
    X(GetFileVersionInfoSizeExW)     \
    X(GetFileVersionInfoSizeW)       \
    X(GetFileVersionInfoW)           \
    X(VerFindFileA)                  \
    X(VerFindFileW)                  \
    X(VerInstallFileA)               \
    X(VerInstallFileW)               \
    X(VerLanguageNameA)              \
    X(VerLanguageNameW)              \
    X(VerQueryValueA)                \
    X(VerQueryValueW)

#define VVR_DECLARE(name) extern "C" FARPROC g_real_##name = nullptr;
VVR_EXPORTS(VVR_DECLARE)
#undef VVR_DECLARE

namespace vvr {

void proxy_resolve_exports() {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\version.dll");
    HMODULE system = LoadLibraryW(path);
    if (system == nullptr) {
        return;
    }
#define VVR_RESOLVE(name) g_real_##name = GetProcAddress(system, #name);
    VVR_EXPORTS(VVR_RESOLVE)
#undef VVR_RESOLVE
}

}  // namespace vvr
