#include "hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>
#include <vector>

#include "log.h"

namespace vvr {

namespace {

uintptr_t g_base = 0;
size_t g_size = 0;
uintptr_t g_code = 0;  // .text
size_t g_code_size = 0;
uintptr_t g_rdata = 0;  // .rdata (string literals)
size_t g_rdata_size = 0;

void resolve_image() {
    if (g_base != 0) {
        return;
    }
    auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
    g_base = reinterpret_cast<uintptr_t>(exe);
    g_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (memcmp(section->Name, ".text", 5) == 0) {
            g_code = g_base + section->VirtualAddress;
            g_code_size = section->Misc.VirtualSize;
        }
        if (memcmp(section->Name, ".rdata", 6) == 0) {
            g_rdata = g_base + section->VirtualAddress;
            g_rdata_size = section->Misc.VirtualSize;
        }
    }
    if (g_code == 0) {
        g_code = g_base;
        g_code_size = g_size;
    }
}

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 1 = must match
};

Pattern parse_pattern(const char* text) {
    Pattern p;
    for (const char* c = text; *c != '\0';) {
        if (*c == ' ') {
            ++c;
            continue;
        }
        if (c[0] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
            c += (c[1] == '?') ? 2 : 1;
            continue;
        }
        char hex[3] = {c[0], c[1], 0};
        p.bytes.push_back(static_cast<uint8_t>(strtoul(hex, nullptr, 16)));
        p.mask.push_back(1);
        c += 2;
    }
    return p;
}

}  // namespace

bool hooks_init() {
    resolve_image();
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("ERROR: MinHook init failed %d", status);
        return false;
    }
    return true;
}

uintptr_t exe_base() {
    resolve_image();
    return g_base;
}

size_t exe_size() {
    resolve_image();
    return g_size;
}

bool in_exe(uintptr_t address) {
    return address >= g_base && address < g_base + g_size;
}

uintptr_t find_pattern(const char* text) {
    resolve_image();
    const Pattern p = parse_pattern(text);
    const size_t n = p.bytes.size();
    if (n == 0 || p.mask[0] == 0) {
        return 0;
    }
    const auto* start = reinterpret_cast<const uint8_t*>(g_code);
    const uint8_t first = p.bytes[0];
    for (size_t i = 0; i + n <= g_code_size;) {
        const void* hit = memchr(start + i, first, g_code_size - n + 1 - i);
        if (hit == nullptr) {
            break;
        }
        i = static_cast<const uint8_t*>(hit) - start;
        size_t j = 1;
        for (; j < n; ++j) {
            if (p.mask[j] != 0 && start[i + j] != p.bytes[j]) {
                break;
            }
        }
        if (j == n) {
            return g_code + i;
        }
        ++i;
    }
    return 0;
}

uintptr_t rip_target(uintptr_t address, int disp_offset, int length) {
    return address + length + *reinterpret_cast<const int32_t*>(address + disp_offset);
}

uintptr_t find_wide_string(const wchar_t* text) {
    resolve_image();
    const size_t len = wcslen(text);
    const auto* w = reinterpret_cast<const wchar_t*>(g_rdata);
    const size_t n = g_rdata_size / sizeof(wchar_t);
    for (size_t i = 1; i + len + 1 < n; ++i) {
        if (w[i] == text[0] && w[i - 1] == 0 && wmemcmp(w + i, text, len) == 0 && w[i + len] == 0) {
            return reinterpret_cast<uintptr_t>(w + i);
        }
    }
    return 0;
}

uintptr_t find_lea_to(uintptr_t target) {
    resolve_image();
    const auto* code = reinterpret_cast<const uint8_t*>(g_code);
    for (size_t i = 0; i + 7 <= g_code_size; ++i) {
        // REX.W (48/4C) 8D modrm with mod=00, rm=101 (RIP-relative)
        if ((code[i] == 0x48 || code[i] == 0x4C) && code[i + 1] == 0x8D && (code[i + 2] & 0xC7) == 0x05) {
            const uintptr_t at = g_code + i;
            if (rip_target(at, 3, 7) == target) {
                return at;
            }
        }
    }
    return 0;
}

bool hook_function(void* target, void* detour, void** original, const char* name) {
    if (target == nullptr) {
        LOG("ERROR: %s not found", name);
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        LOG("ERROR: failed to hook %s at %p", name, target);
        return false;
    }
    if (in_exe(reinterpret_cast<uintptr_t>(target))) {
        LOG("Hooked %s at exe+%llX", name,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) - g_base));
    } else {
        LOG("Hooked %s at %p", name, target);
    }
    return true;
}

bool patch_vtable(void** vtable, int index, void* detour, void** original, const char* name) {
    void** slot = vtable + index;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG("ERROR: VirtualProtect failed for %s", name);
        return false;
    }
    if (original != nullptr) {
        *original = *slot;
    }
    InterlockedExchangePointer(slot, detour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    LOG("Patched %s (vtable slot %d, was exe+%llX)", name, index,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(original ? *original : nullptr) -
                                        g_base));
    return true;
}

bool safe_read(const void* address, void* out, size_t size) {
    __try {
        memcpy(out, address, size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

}  // namespace vvr
