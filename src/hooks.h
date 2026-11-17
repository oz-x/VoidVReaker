#pragma once
#include <cstddef>
#include <cstdint>

namespace vvr {

bool hooks_init();  // MinHook
uintptr_t exe_base();
size_t exe_size();
bool in_exe(uintptr_t address);

// First match of an IDA-style pattern ("48 8B 05 ?? ?? ?? ??") in the executable's code.
uintptr_t find_pattern(const char* pattern);
// Target of a RIP-relative operand: `address` is the instruction, `disp_offset` the offset of
// its disp32 and `length` the instruction length.
uintptr_t rip_target(uintptr_t address, int disp_offset, int length);
// Address of a NUL-preceded UTF-16 string literal in the executable image (0 if absent).
uintptr_t find_wide_string(const wchar_t* text);
// First `lea reg,[rip+disp]` in the code that loads `target` (0 if none).
uintptr_t find_lea_to(uintptr_t target);

// Inline detour (MinHook). Returns false (and logs) on failure.
bool hook_function(void* target, void* detour, void** original, const char* name);
// Replaces one slot of a vtable that lives in read-only data.
bool patch_vtable(void** vtable, int index, void* detour, void** original, const char* name);

// Reads memory that may be unmapped (stale engine pointers). False if not readable.
bool safe_read(const void* address, void* out, size_t size);
template <class T>
inline bool safe_get(const void* address, T* out) {
    return safe_read(address, out, sizeof(T));
}

}  // namespace vvr
