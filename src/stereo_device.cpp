#include "stereo_device.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <mutex>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "render.h"
#include "ue5.h"

using namespace vvr::mathx;

namespace vvr {

namespace {

// FFakeStereoRenderingDevice constructor: stores its two vtables (IStereoRendering at +0,
// IStereoRenderTargetManager at +8).
//   lea rax,[vtable]; movaps [rsp+30h],xmm6; mov [rcx],rax; mov esi,r9d; lea rax,[vtable2]; ...
constexpr const char* FAKE_CTOR_SIG =
    "48 8D 05 ?? ?? ?? ?? 0F 29 74 24 30 48 89 01 41 8B F1 48 8D 05 ?? ?? ?? ?? 41 8B E8 48 89 41 08";

// FSceneViewport constructor: its ISlateViewport vtable is stored at +0xC8.
constexpr const char* SLATE_VIEWPORT_SIG =
    "48 8D 05 ?? ?? ?? ?? 48 89 47 08 48 8D 05 ?? ?? ?? ?? 48 89 47 18 48 8D 05 ?? ?? ?? ?? 48 89 87 C8 00 00 00";
// FSlateRHIRenderer drawing a window: `Viewport->IsStereoscopic3D()` decides whether the UI is
// drawn into the stereo target (then copied by RenderTexture_RenderThread) or into the window.
constexpr const char* SLATE_STEREO_CALL_SIG = "48 8B 03 48 8B CB FF 50 30 84 C0 74 ?? B0 01 EB";
constexpr int kSlateGetRenderTarget = 3;  // ISlateViewport::GetViewportRenderTargetTexture
constexpr int kSlateIsStereoscopic3D = 6;
// FSceneViewport (FViewport base) fields, from FSceneViewport::EnqueueBeginRenderFrame.
constexpr uint32_t kSlateViewportToViewport = 0xC0;  // ISlateViewport subobject - FViewport
constexpr uint32_t kBufferedTargets = 0x2B8;         // TArray<FTextureRHIRef> data, num at +8

// IStereoRendering slots (UE 5.7, checked against the fake device's implementations).
constexpr int kIsStereoEnabled = 1;
constexpr int kGetViewPassForIndex = 5;
constexpr int kAdjustViewRect = 8;
constexpr int kCalculateStereoViewOffset = 11;
constexpr int kGetStereoProjectionMatrix = 12;
// IStereoRenderTargetManager slots.
constexpr int kShouldUseSeparateRenderTarget = 0;
constexpr int kCalculateRenderTargetSize = 2;
constexpr int kNeedReAllocateViewportRenderTarget = 3;
constexpr int kGetNumberOfBufferedFrames = 6;
constexpr int kAllocateRenderTargetTexture = 8;

struct FMatrix {
    double m[4][4];
};

using IsStereoEnabledFn = bool (*)(void*);
using ViewPassFn = int32_t (*)(void*, bool, int32_t);
using AdjustViewRectFn = void (*)(void*, int32_t, int32_t*, int32_t*, uint32_t*, uint32_t*);
using CalcOffsetFn = void (*)(void*, int32_t, ue::FRotator*, float, ue::FVector*);
using ProjectionFn = FMatrix* (*)(void*, FMatrix*, int32_t);
using TargetSizeFn = void (*)(void*, const void*, uint32_t*, uint32_t*);
using NeedReallocFn = bool (*)(void*, const void*);
using SeparateTargetFn = bool (*)(void*);
using BufferedFramesFn = uint32_t (*)(void*);
using SlateIsStereoFn = bool (*)(void*);

IsStereoEnabledFn g_is_stereo_enabled = nullptr;
ViewPassFn g_view_pass = nullptr;
AdjustViewRectFn g_adjust_view_rect = nullptr;
CalcOffsetFn g_calc_offset = nullptr;
ProjectionFn g_projection = nullptr;
TargetSizeFn g_target_size = nullptr;
NeedReallocFn g_need_realloc = nullptr;
SeparateTargetFn g_separate_target = nullptr;
BufferedFramesFn g_buffered_frames = nullptr;
void* g_allocate_target = nullptr;
SlateIsStereoFn g_slate_is_stereo = nullptr;
uintptr_t g_slate_stereo_return = 0;  // return address of the call in the window drawing

std::atomic<bool> g_enabled{false};
std::atomic<int> g_draw_eye{-1};   // eye being drawn, -1: both views in one draw
std::atomic<int> g_draw_view{-1};  // view index that renders it (the other view is dropped)
std::atomic<uint32_t> g_eye_width{1600};
std::atomic<uint32_t> g_eye_height{1600};
std::atomic<uint64_t> g_allocated{0};  // target size last given to the engine (w << 32 | h)
std::atomic<double> g_world_scale{100.0};

std::mutex g_frame_mutex;
EyeFrame g_frame;

uint64_t pack(uint32_t w, uint32_t h) {
    return (static_cast<uint64_t>(w) << 32) | h;
}

// Logs the first few calls of a hook to follow the engine's flow.
bool first_calls(std::atomic<int>& counter) {
    return config().trace && counter.fetch_add(1) < 4;
}

// The eye a stereo view index shows: while one eye is drawn, its view index carries that eye.
int eye_of_view(int32_t view_index) {
    const int eye = g_draw_eye;
    return eye >= 0 && view_index == g_draw_view ? eye : view_index;
}

// A view family holds one eye, so every eye view is a primary stereo view: a secondary view
// would look for its primary in the same family (and crash when there is none).
int32_t hook_view_pass(void* self, bool stereo, int32_t view_index) {
    if (!g_enabled || !stereo) {
        return g_view_pass(self, stereo, view_index);
    }
    return 1;  // EStereoscopicPass::eSSP_PRIMARY
}

bool hook_is_stereo_enabled(void*) {
    return g_enabled.load();
}

// Each eye is drawn as its own frame of the viewport (see stereo_set_draw_eye): the view
// index that does not carry it gets an empty rect, which drops it from that view family.
void hook_adjust_view_rect(void* self, int32_t view_index, int32_t* x, int32_t* y, uint32_t* size_x,
                           uint32_t* size_y) {
    if (!g_enabled || view_index < 0 || view_index > 1) {
        g_adjust_view_rect(self, view_index, x, y, size_x, size_y);
        return;
    }
    const bool drawn = g_draw_eye < 0 || view_index == g_draw_view;
    *x = 0;
    *y = 0;
    *size_x = drawn ? g_eye_width.load() : 0;
    *size_y = drawn ? g_eye_height.load() : 0;
}

void hook_calc_offset(void* self, int32_t view_index, ue::FRotator* rotation, float world_to_meters,
                      ue::FVector* location) {
    EyeFrame f;
    {
        std::lock_guard lock(g_frame_mutex);
        f = g_frame;
    }
    static std::atomic<int> calls{0};
    if (first_calls(calls)) {
        LOG("CalculateStereoViewOffset(%d) at (%.0f %.0f %.0f) yaw %.1f, %g uu/m", view_index,
            location->x, location->y, location->z, rotation->yaw, world_to_meters);
    }
    if (!f.valid || view_index < -1 || view_index > 1) {
        g_calc_offset(self, view_index, rotation, world_to_meters, location);
        return;
    }
    const double scale = config().world_scale > 0.0f ? config().world_scale : world_to_meters;
    g_world_scale = scale;
    const double base_yaw = (f.body_yaw_valid ? f.body_yaw : rotation->yaw) * kDegToRad;
    // INDEX_NONE (mono queries such as screen projection): the point between the eyes.
    Vec3 offset, forward, right, up;
    if (view_index < 0) {
        offset = (f.offset[0] + f.offset[1]) * 0.5;
        forward = f.forward[0];
        right = f.right[0];
        up = f.up[0];
    } else {
        const int eye = eye_of_view(view_index);
        offset = f.offset[eye];
        forward = f.forward[eye];
        right = f.right[eye];
        up = f.up[eye];
    }
    const Vec3 world = yaw_ue(offset * scale, base_yaw);
    location->x += world.x;
    location->y += world.y;
    location->z += world.z;
    const Rotator r = axes_to_rotator(yaw_ue(forward, base_yaw), yaw_ue(right, base_yaw),
                                      yaw_ue(up, base_yaw));
    rotation->pitch = r.pitch;
    rotation->yaw = r.yaw;
    rotation->roll = r.roll;
}

FMatrix* hook_projection(void* self, FMatrix* out, int32_t view_index) {
    g_projection(self, out, view_index);
    EyeFrame f;
    {
        std::lock_guard lock(g_frame_mutex);
        f = g_frame;
    }
    if (!f.valid || view_index < 0 || view_index > 1) {
        return out;
    }
    // Same matrix as FOpenXRHMD::GetStereoProjectionMatrix: reversed-Z, infinite far plane,
    // the near plane taken from the engine's own matrix.
    const double z_near = out->m[3][2];
    const int eye = eye_of_view(view_index);
    const double l = f.tan_left[eye], r = f.tan_right[eye];
    const double u = f.tan_up[eye], d = f.tan_down[eye];
    const double inv_rl = 1.0 / (r - l);
    const double inv_tb = 1.0 / (u - d);
    FMatrix m{};
    m.m[0][0] = 2.0 * inv_rl;
    m.m[1][1] = 2.0 * inv_tb;
    m.m[2][0] = -(r + l) * inv_rl;
    m.m[2][1] = -(u + d) * inv_tb;
    m.m[2][3] = 1.0;
    m.m[3][2] = z_near;
    *out = m;
    return out;
}

// The fake device only asks for its own render target on platforms with mobile multiview;
// forcing it keeps the eyes out of the window (which then holds only the UI).
bool hook_separate_target(void* self) {
    return g_enabled ? true : g_separate_target(self);
}

void hook_target_size(void* self, const void* viewport, uint32_t* x, uint32_t* y) {
    if (!g_enabled) {
        g_target_size(self, viewport, x, y);
        return;
    }
    *x = g_eye_width;
    *y = g_eye_height;
    g_allocated = pack(*x, *y);
    LOG("Eye target size %ux%u", *x, *y);
}

bool hook_need_realloc(void* self, const void* viewport) {
    if (!g_enabled) {
        return g_need_realloc(self, viewport);
    }
    return g_allocated != pack(g_eye_width, g_eye_height);
}

// One target per eye: the viewport rotates through its buffered targets on every draw.
uint32_t hook_buffered_frames(void* self) {
    return g_enabled ? 2 : g_buffered_frames(self);
}

// The device would allocate a 2-slice array (mobile multiview layout); declining makes the
// engine allocate plain 2D targets of CalculateRenderTargetSize.
bool hook_allocate_target(void*, uint32_t, uint32_t, uint32_t, uint8_t, uint32_t, uint64_t, uint64_t,
                          void*, void*, uint32_t) {
    return false;
}

// Slate asks the game viewport whether it is stereo before drawing the window: answering no
// keeps the UI out of the eye targets (it goes to the window, captured at Present). This is
// also the render thread's once-per-frame point where both eye targets are known: the viewport's
// current target holds the last drawn eye (right), the other buffered one the left eye.
bool hook_slate_is_stereo(void* self) {
    if (!g_enabled || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_slate_stereo_return) {
        return g_slate_is_stereo(self);
    }
    using GetTargetFn = void* (*)(void*);
    void* resource = (*reinterpret_cast<GetTargetFn**>(self))[kSlateGetRenderTarget](self);
    void* right = nullptr;  // FRHITexture* (FSlateRenderTargetRHI::RHIRef at +8)
    if (resource != nullptr) {
        safe_get(reinterpret_cast<uint8_t*>(resource) + 8, &right);
    }
    void* left = nullptr;
    const auto* viewport = reinterpret_cast<const uint8_t*>(self) - kSlateViewportToViewport;
    void** targets = nullptr;
    int32_t count = 0;
    if (right != nullptr && safe_get(viewport + kBufferedTargets, &targets) &&
        safe_get(viewport + kBufferedTargets + 8, &count) && count == 2 && targets != nullptr) {
        void* both[2] = {};
        if (safe_read(targets, both, sizeof(both))) {
            left = both[0] == right ? both[1] : both[0];
        }
    }
    static std::atomic<int> calls{0};
    if (first_calls(calls)) {
        LOG("Slate window draw: eye targets %p / %p (%d buffered)", left, right, count);
    }
    render_on_frame_targets(left, right);
    return false;
}

}  // namespace

bool stereo_device_install() {
    const uintptr_t ctor = find_pattern(FAKE_CTOR_SIG);
    const uintptr_t slate_ctor = find_pattern(SLATE_VIEWPORT_SIG);
    const uintptr_t slate_call = find_pattern(SLATE_STEREO_CALL_SIG);
    if (ctor == 0 || slate_ctor == 0 || slate_call == 0) {
        LOG("ERROR: stereo hooks not found (device %d, Slate %d/%d)", ctor != 0, slate_ctor != 0,
            slate_call != 0);
        return false;
    }
    auto** stereo = reinterpret_cast<void**>(rip_target(ctor, 3, 7));
    auto** targets = reinterpret_cast<void**>(rip_target(ctor + 0x12, 3, 7));
    auto** slate = reinterpret_cast<void**>(rip_target(slate_ctor + 0x16, 3, 7));
    g_slate_stereo_return = slate_call + 9;  // after `call [rax+30h]`
    LOG("Stereo device vtables exe+%llX / exe+%llX",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(stereo) - exe_base()),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(targets) - exe_base()));
    bool ok = true;
    ok &= patch_vtable(stereo, kIsStereoEnabled, reinterpret_cast<void*>(&hook_is_stereo_enabled),
                       reinterpret_cast<void**>(&g_is_stereo_enabled), "IsStereoEnabled");
    ok &= patch_vtable(stereo, kGetViewPassForIndex, reinterpret_cast<void*>(&hook_view_pass),
                       reinterpret_cast<void**>(&g_view_pass), "GetViewPassForIndex");
    ok &= patch_vtable(stereo, kAdjustViewRect, reinterpret_cast<void*>(&hook_adjust_view_rect),
                       reinterpret_cast<void**>(&g_adjust_view_rect), "AdjustViewRect");
    ok &= patch_vtable(stereo, kCalculateStereoViewOffset, reinterpret_cast<void*>(&hook_calc_offset),
                       reinterpret_cast<void**>(&g_calc_offset), "CalculateStereoViewOffset");
    ok &= patch_vtable(stereo, kGetStereoProjectionMatrix, reinterpret_cast<void*>(&hook_projection),
                       reinterpret_cast<void**>(&g_projection), "GetStereoProjectionMatrix");
    ok &= patch_vtable(targets, kShouldUseSeparateRenderTarget, reinterpret_cast<void*>(&hook_separate_target),
                       reinterpret_cast<void**>(&g_separate_target), "ShouldUseSeparateRenderTarget");
    ok &= patch_vtable(targets, kCalculateRenderTargetSize, reinterpret_cast<void*>(&hook_target_size),
                       reinterpret_cast<void**>(&g_target_size), "CalculateRenderTargetSize");
    ok &= patch_vtable(targets, kNeedReAllocateViewportRenderTarget,
                       reinterpret_cast<void*>(&hook_need_realloc),
                       reinterpret_cast<void**>(&g_need_realloc), "NeedReAllocateViewportRenderTarget");
    ok &= patch_vtable(targets, kGetNumberOfBufferedFrames, reinterpret_cast<void*>(&hook_buffered_frames),
                       reinterpret_cast<void**>(&g_buffered_frames), "GetNumberOfBufferedFrames");
    ok &= patch_vtable(targets, kAllocateRenderTargetTexture, reinterpret_cast<void*>(&hook_allocate_target),
                       &g_allocate_target, "AllocateRenderTargetTexture");
    ok &= patch_vtable(slate, kSlateIsStereoscopic3D, reinterpret_cast<void*>(&hook_slate_is_stereo),
                       reinterpret_cast<void**>(&g_slate_is_stereo), "ISlateViewport::IsStereoscopic3D");
    return ok;
}

void stereo_set_enabled(bool enabled) {
    if (g_enabled.exchange(enabled) != enabled) {
        LOG("Stereo rendering %s", enabled ? "on" : "off");
    }
}

bool stereo_enabled() {
    return g_enabled;
}

void stereo_set_draw_eye(int eye, int view_index) {
    g_draw_eye = eye;
    g_draw_view = view_index;
}

void stereo_set_eye_size(uint32_t width, uint32_t height) {
    width &= ~1u;
    height &= ~1u;
    if (g_eye_width != width || g_eye_height != height) {
        LOG("Eye size %ux%u", width, height);
    }
    g_eye_width = width;
    g_eye_height = height;
}

void stereo_eye_size(uint32_t* width, uint32_t* height) {
    *width = g_eye_width;
    *height = g_eye_height;
}

void stereo_set_frame(const EyeFrame& frame) {
    std::lock_guard lock(g_frame_mutex);
    g_frame = frame;
}

double stereo_world_scale() {
    return g_world_scale;
}

}  // namespace vvr
