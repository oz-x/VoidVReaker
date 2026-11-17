#include "ue5.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hooks.h"
#include "log.h"

namespace ue {

namespace {

// FUObjectArray::ObjObjects.Objects inside FUObjectArray::IndexToObject:
//   mov rax,[Objects]; mov rcx,[rax+rcx*8]; lea rax,[rcx+rdx*8]
constexpr const char* OBJECTS_SIG = "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1";
// FName pool construction: lea rcx,[NamePoolData]; call FNamePool::FNamePool; mov [init],1
constexpr const char* NAMEPOOL_SIG = "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 0F 28";
// UGameViewportClient::Draw: mov rcx,[GEngine]; mov rdx,r12; call IsStereoscopic3D; ...
constexpr const char* GENGINE_SIG = "48 8B 0D ?? ?? ?? ?? 49 8B D4 E8 ?? ?? ?? ?? 49 8B 0C 24 0F B6 D8";
constexpr int kProcessEventIndex = 0x260 / 8;

struct ChunkedArray {
    uint8_t** chunks;      // FUObjectItem chunks of 64K items
    void* preallocated;
    int32_t max_elements;
    int32_t num_elements;
    int32_t max_chunks;
    int32_t num_chunks;
};
constexpr int kItemSize = 24;  // FUObjectItem

ChunkedArray* g_objects = nullptr;
uint8_t** g_name_blocks = nullptr;  // FNamePool::Blocks
UObject** g_engine = nullptr;

bool read_name(FName name, std::string* out) {
    const uint32_t block = name.index >> 16;
    const uint32_t offset = (name.index & 0xFFFF) * 2;
    if (g_name_blocks == nullptr || block >= 8192 || g_name_blocks[block] == nullptr) {
        return false;
    }
    const uint8_t* entry = g_name_blocks[block] + offset;
    const uint16_t header = *reinterpret_cast<const uint16_t*>(entry);
    const uint32_t len = header >> 6;
    const bool wide = (header & 1) != 0;
    out->clear();
    if (wide) {
        const auto* w = reinterpret_cast<const wchar_t*>(entry + 2);
        for (uint32_t i = 0; i < len; ++i) {
            out->push_back(static_cast<char>(w[i] < 128 ? w[i] : '?'));
        }
    } else {
        out->assign(reinterpret_cast<const char*>(entry + 2), len);
    }
    return true;
}

UObject* item_object(int32_t index) {
    uint8_t* chunk = g_objects->chunks[index >> 16];
    if (chunk == nullptr) {
        return nullptr;
    }
    // FUObjectItem (5.7): packed flags/refcount, Object, cluster root and serial number.
    return *reinterpret_cast<UObject**>(chunk + (index & 0xFFFF) * kItemSize + 8);
}

int32_t read_internal_index(const UObject* obj) {
    int32_t index = -1;
    if (!vvr::safe_get(reinterpret_cast<const uint8_t*>(obj) + off::object_internal_index, &index)) {
        return -1;
    }
    return index;
}

// FNamePool: CurrentBlock and CurrentByteCursor sit right before Blocks.
uint32_t pool_current_block() {
    return reinterpret_cast<const uint32_t*>(g_name_blocks)[-2];
}
uint32_t pool_current_cursor() {
    return reinterpret_cast<const uint32_t*>(g_name_blocks)[-1];
}

// Name text -> FName comparison index (case-insensitive, like FName itself).
std::mutex g_names_mutex;
std::unordered_map<std::string, uint32_t> g_name_indices;
std::unordered_map<std::string, DWORD> g_name_misses;  // last failed scan

std::string lower(const char* text) {
    std::string s = text;
    for (char& c : s) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool scan_name_pool(const std::string& wanted, uint32_t* out) {
    const uint32_t blocks = pool_current_block();
    for (uint32_t b = 0; b <= blocks && b < 8192; ++b) {
        const uint8_t* block = g_name_blocks[b];
        if (block == nullptr) {
            break;
        }
        const uint32_t limit = b == blocks ? pool_current_cursor() : 0x20000;
        for (uint32_t offset = 0; offset + 2 <= limit;) {
            const uint16_t header = *reinterpret_cast<const uint16_t*>(block + offset);
            const uint32_t len = header >> 6;
            const bool wide = (header & 1) != 0;
            if (len == 0) {
                break;
            }
            if (!wide && len == wanted.size() &&
                _strnicmp(reinterpret_cast<const char*>(block + offset + 2), wanted.c_str(), len) == 0) {
                *out = (b << 16) | (offset >> 1);
                return true;
            }
            offset += (2 + len * (wide ? 2 : 1) + 1) & ~1u;
        }
    }
    return false;
}

bool name_index(const char* text, uint32_t* out) {
    const std::string key = lower(text);
    std::lock_guard lock(g_names_mutex);
    if (auto it = g_name_indices.find(key); it != g_name_indices.end()) {
        *out = it->second;
        return true;
    }
    if (auto it = g_name_misses.find(key); it != g_name_misses.end() && GetTickCount() - it->second < 2000) {
        return false;
    }
    if (scan_name_pool(key, out)) {
        g_name_indices[key] = *out;
        return true;
    }
    g_name_misses[key] = GetTickCount();
    return false;
}

// Class of a possibly half-constructed object, read defensively.
const UStruct* safe_class(const UObject* obj) {
    const UStruct* cls = nullptr;
    return vvr::safe_get(reinterpret_cast<const uint8_t*>(obj) + off::object_class, &cls) ? cls : nullptr;
}

const UStruct* safe_super(const UStruct* s) {
    const UStruct* super = nullptr;
    return vvr::safe_get(reinterpret_cast<const uint8_t*>(s) + off::struct_super, &super) ? super : nullptr;
}

bool safe_name(const UObject* obj, FName* out) {
    return vvr::safe_get(reinterpret_cast<const uint8_t*>(obj) + off::object_name, out);
}

}  // namespace

bool init() {
    if (g_objects != nullptr) {
        return true;
    }
    const uintptr_t objects = vvr::find_pattern(OBJECTS_SIG);
    const uintptr_t names = vvr::find_pattern(NAMEPOOL_SIG);
    const uintptr_t engine = vvr::find_pattern(GENGINE_SIG);
    if (objects == 0 || names == 0 || engine == 0) {
        LOG("ERROR: UE signatures missing (objects %d, names %d, engine %d)", objects != 0,
            names != 0, engine != 0);
        return false;
    }
    g_objects = reinterpret_cast<ChunkedArray*>(vvr::rip_target(objects, 3, 7));
    g_name_blocks = reinterpret_cast<uint8_t**>(vvr::rip_target(names, 3, 7) + 0x10);
    g_engine = reinterpret_cast<UObject**>(vvr::rip_target(engine, 3, 7));
    LOG("UE: objects exe+%llX, names exe+%llX, GEngine exe+%llX",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_objects) - vvr::exe_base()),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_name_blocks) - vvr::exe_base()),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_engine) - vvr::exe_base()));
    return true;
}

bool ready() {
    return g_objects != nullptr && g_objects->num_elements > 0 && g_name_blocks != nullptr &&
           g_name_blocks[0] != nullptr;
}

UObject* engine() {
    return g_engine != nullptr ? *g_engine : nullptr;
}

std::string name_string(FName name) {
    std::string out;
    if (!read_name(name, &out)) {
        return {};
    }
    if (name.number > 0) {
        out += "_" + std::to_string(name.number - 1);
    }
    return out;
}

bool name_is(FName name, const char* text) {
    uint32_t index = 0;
    return name.number == 0 && name_index(text, &index) && name.index == index;
}

std::string object_name(const UObject* obj) {
    return obj == nullptr ? std::string("None") : name_string(name_of(obj));
}

std::string full_name(const UObject* obj) {
    if (obj == nullptr) {
        return "None";
    }
    std::string path = object_name(obj);
    for (const UObject* o = outer_of(obj); o != nullptr; o = outer_of(o)) {
        path = object_name(o) + "." + path;
    }
    return object_name(class_of(obj)) + " " + path;
}

int32_t object_count() {
    return g_objects == nullptr ? 0 : g_objects->num_elements;
}

UObject* object_at(int32_t index) {
    if (g_objects == nullptr || index < 0 || index >= g_objects->num_elements) {
        return nullptr;
    }
    return item_object(index);
}

bool is_live(const UObject* obj) {
    if (obj == nullptr || g_objects == nullptr) {
        return false;
    }
    // The pointer may be stale (even freed): only trust it if GUObjectArray still maps its
    // slot back to it.
    const int32_t index = read_internal_index(obj);
    return index >= 0 && index < g_objects->num_elements && item_object(index) == obj;
}

bool is_default_object(const UObject* obj) {
    return (field<uint32_t>(obj, off::object_flags) & (RF_ClassDefaultObject | RF_ArchetypeObject)) != 0;
}

bool is_a(const UObject* obj, const char* class_name) {
    uint32_t wanted = 0;
    if (obj == nullptr || !name_index(class_name, &wanted)) {
        return false;
    }
    int depth = 0;
    for (const UStruct* c = safe_class(obj); c != nullptr && depth < 64; c = safe_super(c), ++depth) {
        FName name;
        if (!safe_name(c, &name)) {
            return false;
        }
        if (name.index == wanted && name.number == 0) {
            return true;
        }
    }
    return false;
}

bool is_a(const UObject* obj, const UClass* cls) {
    int depth = 0;
    for (const UStruct* c = safe_class(obj); c != nullptr && depth < 64; c = safe_super(c), ++depth) {
        if (c == cls) {
            return true;
        }
    }
    return false;
}

UClass* find_class(const char* name) {
    uint32_t wanted = 0;
    UClass* class_class = nullptr;
    if (!name_index(name, &wanted)) {
        return nullptr;
    }
    for (int32_t i = 0; i < object_count(); ++i) {
        UObject* obj = item_object(i);
        FName n;
        if (obj == nullptr || !safe_name(obj, &n) || n.index != wanted || n.number != 0) {
            continue;
        }
        // A class object's own class derives from UClass ("Class", "BlueprintGeneratedClass"...).
        if (class_class == nullptr) {
            for (int32_t j = 0; j < object_count() && class_class == nullptr; ++j) {
                UObject* c = item_object(j);
                FName cn;
                if (c != nullptr && safe_name(c, &cn) && name_is(cn, "Class") && safe_class(c) == c) {
                    class_class = c;
                }
            }
        }
        if (class_class != nullptr && is_a(safe_class(obj), class_class)) {
            return obj;
        }
    }
    return nullptr;
}

UObject* find_first_instance(const char* class_name) {
    UClass* cls = find_class(class_name);
    if (cls == nullptr) {
        return nullptr;
    }
    for (int32_t i = 0; i < object_count(); ++i) {
        UObject* obj = item_object(i);
        if (obj != nullptr && is_a(obj, cls) && !is_default_object(obj)) {
            return obj;
        }
    }
    return nullptr;
}

FProperty* first_property(const UStruct* type) {
    return field<FProperty*>(type, off::struct_child_properties);
}

FProperty* next_property(const FProperty* prop) {
    return field<FProperty*>(prop, off::ffield_next);
}

std::string property_name(const FProperty* prop) {
    return name_string(field<FName>(prop, off::ffield_name));
}

std::string property_class_name(const FProperty* prop) {
    const void* cls = field<void*>(prop, off::ffield_class);
    return cls == nullptr ? std::string() : name_string(field<FName>(cls, 0));
}

FProperty* find_property(const UStruct* type, const char* name) {
    for (const UStruct* s = type; s != nullptr; s = super_of(s)) {
        for (FProperty* p = first_property(s); p != nullptr; p = next_property(p)) {
            if (name_is(field<FName>(p, off::ffield_name), name)) {
                return p;
            }
        }
    }
    return nullptr;
}

int32_t property_offset(const UStruct* type, const char* name) {
    FProperty* prop = find_property(type, name);
    return prop == nullptr ? -1 : field<int32_t>(prop, off::property_offset);
}

BoolRef find_bool(const UStruct* type, const char* name) {
    BoolRef out;
    FProperty* prop = find_property(type, name);
    if (prop != nullptr && property_class_name(prop) == "BoolProperty") {
        out.offset = field<int32_t>(prop, off::property_offset) + field<uint8_t>(prop, off::bool_byte_offset);
        out.mask = field<uint8_t>(prop, off::bool_field_mask);
    }
    return out;
}

UFunction* find_function(const UStruct* type, const char* name) {
    for (const UStruct* s = type; s != nullptr; s = super_of(s)) {
        for (UObject* f = field<UObject*>(s, off::struct_children); f != nullptr;
             f = field<UObject*>(f, off::field_next)) {
            if (name_is(name_of(f), name)) {
                return f;
            }
        }
    }
    return nullptr;
}

uint32_t params_size(const UFunction* func) {
    return field<uint32_t>(func, off::struct_properties_size);
}

void call_function(UObject* obj, UFunction* func, void* params) {
    using ProcessEventFn = void (*)(UObject*, UFunction*, void*);
    auto process_event = (*reinterpret_cast<ProcessEventFn**>(obj))[kProcessEventIndex];
    process_event(obj, func, params);
}

bool set_int_cvar(const wchar_t* name, int value, int min, int max) {
    const uintptr_t text = vvr::find_wide_string(name);
    const uintptr_t lea_name = text != 0 ? vvr::find_lea_to(text) : 0;
    if (lea_name == 0) {
        LOG("cvar %ls: registration not found", name);
        return false;
    }
    // The static initializer around the name: `lea r8,[storage]` (FAutoConsoleVariableRef) or
    // `lea rcx,[TAutoConsoleVariable]` whose Ref (+0x10) points at the value.
    int32_t* values = nullptr;
    int count = 0;
    for (uintptr_t p = lea_name - 0x20; p < lea_name + 0x20; ++p) {
        const auto* b = reinterpret_cast<const uint8_t*>(p);
        if (b[0] == 0x4C && b[1] == 0x8D && b[2] == 0x05 && p < lea_name) {
            values = reinterpret_cast<int32_t*>(vvr::rip_target(p, 3, 7));
            count = 1;
        }
        if (b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x0D && p > lea_name && values == nullptr) {
            const uintptr_t object = vvr::rip_target(p, 3, 7);
            int32_t* ref = nullptr;
            if (vvr::safe_get(reinterpret_cast<void*>(object + 0x10), &ref) && ref != nullptr) {
                values = ref;  // TConsoleVariableData<int32>::ShadowedValue[2]
                count = 2;
            }
            break;
        }
    }
    int32_t current[2] = {};
    if (values == nullptr || !vvr::safe_read(values, current, sizeof(int32_t) * count)) {
        LOG("cvar %ls: storage not found", name);
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (current[i] < min || current[i] > max) {
            LOG("cvar %ls: unexpected value %d, not changed", name, current[i]);
            return false;
        }
    }
    for (int i = 0; i < count; ++i) {
        values[i] = value;
    }
    LOG("cvar %ls: %d -> %d", name, current[0], value);
    return true;
}

UObject* find_default_object(const char* class_name) {
    const std::string wanted = std::string("Default__") + class_name;
    uint32_t index = 0;
    if (!name_index(wanted.c_str(), &index)) {
        return nullptr;
    }
    for (int32_t i = 0; i < object_count(); ++i) {
        UObject* obj = item_object(i);
        FName n;
        if (obj != nullptr && safe_name(obj, &n) && n.index == index && n.number == 0) {
            return obj;
        }
    }
    return nullptr;
}

void console_command(UObject* player_controller, const wchar_t* command) {
    // UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, Command, SpecificPlayer)
    static UObject* library = nullptr;
    static UFunction* func = nullptr;
    if (func == nullptr) {
        library = find_default_object("KismetSystemLibrary");
        func = library != nullptr ? find_function(class_of(library), "ExecuteConsoleCommand") : nullptr;
    }
    if (func == nullptr) {
        LOG("console_command: ExecuteConsoleCommand not found");
        return;
    }
    std::vector<uint8_t> params(params_size(func) + 16, 0);
    const int32_t world_off = property_offset(func, "WorldContextObject");
    const int32_t cmd_off = property_offset(func, "Command");
    const int32_t player_off = property_offset(func, "SpecificPlayer");
    if (world_off < 0 || cmd_off < 0) {
        LOG("console_command: unexpected parameters");
        return;
    }
    std::wstring copy = command;
    field<UObject*>(params.data(), world_off) = player_controller;
    auto& str = field<FString>(params.data(), cmd_off);
    str.data = copy.data();
    str.count = static_cast<int32_t>(copy.size() + 1);
    str.max = str.count;
    if (player_off >= 0) {
        field<UObject*>(params.data(), player_off) = player_controller;
    }
    LOG("console: %ls", command);
    call_function(library, func, params.data());
}

Call::Call(UObject* obj, const char* function) : obj_(obj) {
    if (obj == nullptr || !is_live(obj)) {
        return;
    }
    // Game thread only.
    static std::unordered_map<std::string, UFunction*> cache;
    UClass* cls = class_of(obj);
    const std::string key = std::to_string(reinterpret_cast<uintptr_t>(cls)) + function;
    const auto it = cache.find(key);
    if (it != cache.end()) {
        func_ = it->second;
    } else {
        func_ = find_function(cls, function);
        cache.emplace(key, func_);
        if (func_ == nullptr) {
            LOG("UFunction %s not found on %s", function, object_name(cls).c_str());
        } else if (params_size(func_) > sizeof(params_)) {
            LOG("UFunction %s: parameters too large", function);
            func_ = nullptr;
            cache[key] = nullptr;
        }
    }
}

bool Call::run() {
    if (func_ == nullptr) {
        return false;
    }
    call_function(obj_, func_, params_);
    return true;
}

}  // namespace ue
