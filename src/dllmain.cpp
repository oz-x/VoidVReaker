#include <windows.h>

#include <cwchar>

#include "proxy.h"
#include "vr.h"

namespace {

// Only the game itself gets the mod (other tools may load version.dll from this folder).
bool is_game_process() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;
    return _wcsnicmp(name, L"VOIDBREAKER", 11) == 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        vvr::proxy_resolve_exports();
        if (is_game_process()) {
            vvr::vr_early_init();
        }
    }
    return TRUE;
}
