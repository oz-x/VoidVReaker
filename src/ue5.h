#pragma once
// Minimal Unreal Engine 5.7 reflection (GUObjectArray, FNamePool, FProperty, ProcessEvent).
// Layout facts: Dumper-7 / UE4SS conventions, checked against VOID/BREAKER's executable.
#include <cstdint>
#include <string>

namespace ue {

struct FName {
    uint32_t index = 0;
    uint32_t number = 0;
};

template <class T>
struct TArray {
    T* data;
    int32_t count;
    int32_t max;
};

using FString = TArray<wchar_t>;

struct FVector {
    double x, y, z;
};

struct FRotator {
    double pitch, yaw, roll;
};

struct UObject;  // opaque; accessed through the helpers below
using UClass = UObject;
using UStruct = UObject;
using UFunction = UObject;
struct FProperty;

namespace off {
constexpr uint32_t object_flags = 0x08;
constexpr uint32_t object_internal_index = 0x0C;
constexpr uint32_t object_class = 0x10;
constexpr uint32_t object_name = 0x18;
constexpr uint32_t object_outer = 0x20;
constexpr uint32_t field_next = 0x28;            // UField::Next
constexpr uint32_t struct_super = 0x40;
constexpr uint32_t struct_children = 0x48;       // UField* (functions)
constexpr uint32_t struct_child_properties = 0x50;  // FField*
constexpr uint32_t struct_properties_size = 0x58;
constexpr uint32_t function_flags = 0xB0;
// FField in 5.7: vtable, ClassPrivate, Owner (one pointer, UObject bit packed), Next, Name.
constexpr uint32_t ffield_class = 0x08;
constexpr uint32_t ffield_next = 0x18;
constexpr uint32_t ffield_name = 0x20;
constexpr uint32_t property_array_dim = 0x30;
constexpr uint32_t property_element_size = 0x34;
constexpr uint32_t property_offset = 0x44;
// sizeof(FProperty) = 0x70: subclass fields start there.
constexpr uint32_t bool_byte_offset = 0x71;
constexpr uint32_t bool_field_mask = 0x73;
constexpr uint32_t object_property_class = 0x70;
constexpr uint32_t struct_property_struct = 0x70;
}  // namespace off

constexpr uint32_t RF_ClassDefaultObject = 0x10;
constexpr uint32_t RF_ArchetypeObject = 0x20;

template <class T>
inline T& field(const void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
}

inline UClass* class_of(const UObject* obj) {
    return field<UClass*>(obj, off::object_class);
}
inline UObject* outer_of(const UObject* obj) {
    return field<UObject*>(obj, off::object_outer);
}
inline FName name_of(const UObject* obj) {
    return field<FName>(obj, off::object_name);
}
inline UStruct* super_of(const UStruct* s) {
    return field<UStruct*>(s, off::struct_super);
}

// Resolves GUObjectArray, the name pool and GEngine. Call once the executable is mapped.
bool init();
bool ready();
UObject* engine();  // GEngine

// Name helpers.
std::string name_string(FName name);
std::string object_name(const UObject* obj);
std::string full_name(const UObject* obj);  // "ClassName Outer.Outer.Name"
bool name_is(FName name, const char* text);

// Object lookup / liveness.
int32_t object_count();
UObject* object_at(int32_t index);
bool is_live(const UObject* obj);
bool is_default_object(const UObject* obj);
bool is_a(const UObject* obj, const char* class_name);
bool is_a(const UObject* obj, const UClass* cls);
UClass* find_class(const char* name);
// First live, non-template instance whose class (or a superclass) is called `class_name`.
UObject* find_first_instance(const char* class_name);
// The class default object (CDO) of `class_name`, e.g. to call static UFunctions.
UObject* find_default_object(const char* class_name);

// Reflection.
FProperty* find_property(const UStruct* type, const char* name);
int32_t property_offset(const UStruct* type, const char* name);  // -1 if missing
std::string property_class_name(const FProperty* prop);
std::string property_name(const FProperty* prop);
FProperty* next_property(const FProperty* prop);
FProperty* first_property(const UStruct* type);

// A resolved bool property (one bit of one byte).
struct BoolRef {
    int32_t offset = -1;
    uint8_t mask = 0;
    bool valid() const { return offset >= 0; }
    bool get(const void* obj) const {
        return valid() && (field<uint8_t>(obj, offset) & mask) != 0;
    }
    void set(void* obj, bool value) const {
        if (valid()) {
            uint8_t& bits = field<uint8_t>(obj, offset);
            bits = value ? (bits | mask) : (bits & ~mask);
        }
    }
};
BoolRef find_bool(const UStruct* type, const char* name);

// Sets an int console variable registered by a static TAutoConsoleVariable<int32> or
// FAutoConsoleVariableRef, found through its name literal. Values outside [min, max] in memory
// mean the layout is not the expected one: nothing is written.
bool set_int_cvar(const wchar_t* name, int value, int min, int max);

UFunction* find_function(const UStruct* type, const char* name);
uint32_t params_size(const UFunction* func);
// Calls a UFunction through ProcessEvent.
void call_function(UObject* obj, UFunction* func, void* params);
// Runs a console command through APlayerController::ConsoleCommand.
void console_command(UObject* player_controller, const wchar_t* command);

// One UFunction call on `obj` by name, parameters set and read by name. Functions are cached
// per class, so per-frame calls are cheap.
class Call {
public:
    Call(UObject* obj, const char* function);
    bool ok() const { return func_ != nullptr; }
    template <class T>
    Call& set(const char* name, const T& value) {
        const int32_t o = ok() ? property_offset(func_, name) : -1;
        if (o >= 0) {
            field<T>(params_, o) = value;
        }
        return *this;
    }
    template <class T>
    T get(const char* name) const {
        const int32_t o = ok() ? property_offset(func_, name) : -1;
        return o >= 0 ? field<T>(params_, o) : T{};
    }
    bool run();
    // Raw parameter memory and its size (structs passed through unchanged); null if missing.
    uint8_t* param(const char* name, uint32_t* size = nullptr) {
        FProperty* prop = ok() ? find_property(func_, name) : nullptr;
        if (prop == nullptr) {
            return nullptr;
        }
        if (size != nullptr) {
            *size = field<uint32_t>(prop, off::property_element_size);
        }
        return params_ + field<int32_t>(prop, off::property_offset);
    }

private:
    UObject* obj_ = nullptr;
    UFunction* func_ = nullptr;
    alignas(16) uint8_t params_[1024] = {};
};

}  // namespace ue
