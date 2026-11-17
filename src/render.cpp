#include "render.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "frame.h"
#include "hooks.h"
#include "log.h"
#include "stereo_device.h"
#include "vr.h"
#include "xr.h"

using Microsoft::WRL::ComPtr;

namespace vvr {

namespace {

// ---- DXGI hooks ----
using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                              const DXGI_SWAP_CHAIN_DESC1*,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                              IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                                const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT,
                                                     UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT,
                                                      UINT, const UINT*, IUnknown* const*);

CreateFactory1Fn g_create_factory1 = nullptr;
CreateFactory2Fn g_create_factory2 = nullptr;
CreateSwapChainForHwndFn g_create_swapchain = nullptr;
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resize = nullptr;
ResizeBuffers1Fn g_resize1 = nullptr;

std::atomic<bool> g_engine_init_done{false};

// ---- the game's swapchain ----
IDXGISwapChain3* g_swapchain = nullptr;  // not owned
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
std::atomic<uint32_t> g_bb_width{0};
std::atomic<uint32_t> g_bb_height{0};

constexpr int kMaxBuffers = 8;
ComPtr<ID3D12Resource> g_backbuffers[kMaxBuffers];
ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
UINT g_rtv_size = 0;

// Our own command lists on the game's queue (back buffer clears, mirror, dumps).
constexpr int kRing = 3;
ComPtr<ID3D12CommandAllocator> g_allocators[kRing];
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
uint64_t g_fence_values[kRing]{};
uint64_t g_fence_next = 1;
HANDLE g_fence_event = nullptr;
int g_ring_index = 0;
std::mutex g_list_mutex;

// ---- frames in flight ----
struct RenderedFrame {
    bool has_slot = false;
    FrameSlot slot;
    ComPtr<ID3D12Resource> eyes[2];  // the engine's eye targets of this frame (left, right)
};
std::mutex g_frames_mutex;
std::deque<FrameSlot> g_game_frames;      // game thread -> render thread
std::deque<RenderedFrame> g_rendered;     // render thread -> Present

std::atomic<bool> g_dump_requested{false};

// ---- stereo target discovery ----
std::vector<void*> g_resource_vtables;  // vtables of ID3D12Resource implementations
struct TargetPath {
    int depth = 0;
    uint32_t offsets[4]{};
};
TargetPath g_target_path;
bool g_target_path_valid = false;

// ---- the engine's UI target ----
// The engine draws the window (Slate: the UI only, the eyes go to their own targets) into an
// intermediate texture per back buffer and copies it to the back buffer before Present. Slate
// never clears it (in flat play the game view covers it), so right after that copy, on the
// engine's own command list, it is cleared to transparent black: the next frame's UI then
// lands on alpha 0 and can be cut out with its alpha.
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
CopyResourceFn g_cl_copy = nullptr;
constexpr int kUiTargets = 4;
ID3D12Resource* g_ui_targets[kUiTargets]{};  // not owned; RTVs at heap index kMaxBuffers + i
int g_ui_target_next = 0;
bool is_backbuffer(ID3D12Resource* r);

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

D3D12_CPU_DESCRIPTOR_HANDLE ui_target_rtv(ID3D12Resource* target) {
    int slot = -1;
    for (int i = 0; i < kUiTargets; ++i) {
        if (g_ui_targets[i] == target) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = g_ui_target_next;
        g_ui_target_next = (g_ui_target_next + 1) % kUiTargets;
        g_ui_targets[slot] = target;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(kMaxBuffers + slot) * g_rtv_size;
        g_device->CreateRenderTargetView(target, nullptr, rtv);
        const D3D12_RESOURCE_DESC d = target->GetDesc();
        LOG("Engine UI target %p (%llux%u format %d): cleared after each copy to the window", (void*)target,
            d.Width, d.Height, d.Format);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kMaxBuffers + slot) * g_rtv_size;
    return rtv;
}

void STDMETHODCALLTYPE cl_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src) {
    g_cl_copy(list, dst, src);
    if (!stereo_enabled() || list == g_list.Get() || src == dst || !is_backbuffer(dst)) {
        return;
    }
    const D3D12_RESOURCE_DESC d = src->GetDesc();
    if ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 || d.DepthOrArraySize != 1) {
        return;
    }
    std::lock_guard lock(g_list_mutex);  // the RTV slots
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = ui_target_rtv(src);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    transition(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    transition(list, src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

void hook_copy_resource(ID3D12GraphicsCommandList* list) {
    void** vt = *reinterpret_cast<void***>(list);  // the same for every command list of the device
    hook_function(vt[17], reinterpret_cast<void*>(&cl_copy), reinterpret_cast<void**>(&g_cl_copy),
                  "ID3D12GraphicsCommandList::CopyResource");
}

bool is_backbuffer(ID3D12Resource* r) {
    if (r == nullptr) {
        return false;
    }
    for (const auto& b : g_backbuffers) {
        if (b.Get() == r) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------

bool create_ring() {
    for (auto& allocator : g_allocators) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&allocator)))) {
            return false;
        }
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(),
                                           nullptr, IID_PPV_ARGS(&g_list)))) {
        return false;
    }
    g_list->Close();
    hook_copy_resource(g_list.Get());
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        return false;
    }
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = kMaxBuffers + kUiTargets;
    if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_rtv_heap)))) {
        return false;
    }
    g_rtv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

void ring_begin() {
    const int index = g_ring_index;
    if (g_fence->GetCompletedValue() < g_fence_values[index]) {
        g_fence->SetEventOnCompletion(g_fence_values[index], g_fence_event);
        WaitForSingleObject(g_fence_event, 1000);
    }
    g_allocators[index]->Reset();
    g_list->Reset(g_allocators[index].Get(), nullptr);
}

uint64_t ring_submit() {
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    const int index = g_ring_index;
    g_ring_index = (g_ring_index + 1) % kRing;
    g_fence_values[index] = g_fence_next++;
    g_queue->Signal(g_fence.Get(), g_fence_values[index]);
    return g_fence_values[index];
}

void ring_wait(uint64_t value) {
    if (g_fence->GetCompletedValue() < value) {
        g_fence->SetEventOnCompletion(value, g_fence_event);
        WaitForSingleObject(g_fence_event, 2000);
    }
}

void ring_wait_all() {
    ring_wait(g_fence_next - 1);
}

void barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    g_list->ResourceBarrier(1, &b);
}

ID3D12Resource* backbuffer(UINT index) {
    if (index >= kMaxBuffers || g_swapchain == nullptr) {
        return nullptr;
    }
    if (!g_backbuffers[index]) {
        if (FAILED(g_swapchain->GetBuffer(index, IID_PPV_ARGS(&g_backbuffers[index])))) {
            return nullptr;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(index) * g_rtv_size;
        g_device->CreateRenderTargetView(g_backbuffers[index].Get(), nullptr, rtv);
        const D3D12_RESOURCE_DESC desc = g_backbuffers[index]->GetDesc();
        g_bb_width = static_cast<uint32_t>(desc.Width);
        g_bb_height = desc.Height;
    }
    return g_backbuffers[index].Get();
}

void release_backbuffers() {
    std::lock_guard lock(g_list_mutex);
    if (g_fence) {
        ring_wait_all();
    }
    for (auto& b : g_backbuffers) {
        b.Reset();
    }
}

// Desktop window: the middle of the left eye (no UI).
void mirror_left_eye(ID3D12Resource* eyes, D3D12_RESOURCE_STATES eyes_state, ID3D12Resource* bb) {
    const D3D12_RESOURCE_DESC src = eyes->GetDesc();
    const D3D12_RESOURCE_DESC dst = bb->GetDesc();
    if ((src.Format == DXGI_FORMAT_R10G10B10A2_UNORM) != (dst.Format == DXGI_FORMAT_R10G10B10A2_UNORM)) {
        return;  // incompatible copy
    }
    const UINT eye_w = static_cast<UINT>(src.Width);
    const UINT w = std::min<UINT>(eye_w, static_cast<UINT>(dst.Width));
    const UINT h = std::min<UINT>(src.Height, dst.Height);
    const UINT sx = (eye_w - w) / 2, sy = (src.Height - h) / 2;
    const UINT dx = (static_cast<UINT>(dst.Width) - w) / 2, dy = (dst.Height - h) / 2;
    std::lock_guard lock(g_list_mutex);
    ring_begin();
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(eyes, eyes_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to{bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION from{eyes, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    const D3D12_BOX box{sx, sy, 0, sx + w, sy + h, 1};
    g_list->CopyTextureRegion(&to, dx, dy, 0, &from, &box);
    barrier(eyes, D3D12_RESOURCE_STATE_COPY_SOURCE, eyes_state);
    barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ring_submit();
}

// ---- debug dumps (TGA, keeps the UI alpha) ----
void write_tga(const std::wstring& path, const uint8_t* bgra, UINT width, UINT height, UINT pitch) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f == nullptr) {
        return;
    }
    uint8_t header[18] = {};
    header[2] = 2;  // uncompressed true colour
    header[12] = static_cast<uint8_t>(width);
    header[13] = static_cast<uint8_t>(width >> 8);
    header[14] = static_cast<uint8_t>(height);
    header[15] = static_cast<uint8_t>(height >> 8);
    header[16] = 32;
    header[17] = 0x28;  // 8 alpha bits, top-left origin
    fwrite(header, 1, sizeof(header), f);
    for (UINT y = 0; y < height; ++y) {
        fwrite(bgra + static_cast<size_t>(y) * pitch, 4, width, f);
    }
    fclose(f);
    LOG("Dumped %ls (%ux%u)", path.c_str(), width, height);
}

void dump_resource(ID3D12Resource* resource, D3D12_RESOURCE_STATES state, const wchar_t* name,
                   UINT subresource = 0) {
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&readback)))) {
        LOG("Dump: readback buffer failed");
        return;
    }
    uint64_t fence = 0;
    {
        std::lock_guard lock(g_list_mutex);
        ring_begin();
        barrier(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        from.SubresourceIndex = subresource;
        g_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        fence = ring_submit();
        ring_wait(fence);
    }
    uint8_t* data = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&data)))) {
        return;
    }
    const UINT width = static_cast<UINT>(desc.Width), height = desc.Height;
    const UINT pitch = footprint.Footprint.RowPitch;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * pitch;
        uint8_t* out = pixels.data() + static_cast<size_t>(y) * width * 4;
        for (UINT x = 0; x < width; ++x) {
            const uint8_t* p = row + x * 4;
            switch (desc.Format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    out[x * 4 + 0] = p[2];
                    out[x * 4 + 1] = p[1];
                    out[x * 4 + 2] = p[0];
                    out[x * 4 + 3] = p[3];
                    break;
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
                    const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
                    out[x * 4 + 2] = static_cast<uint8_t>((v & 0x3FF) >> 2);
                    out[x * 4 + 1] = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
                    out[x * 4 + 0] = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
                    out[x * 4 + 3] = static_cast<uint8_t>((v >> 30) * 85);
                    break;
                }
                default:
                    memcpy(out + x * 4, p, 4);
                    break;
            }
        }
    }
    readback->Unmap(0, nullptr);
    LOG("Dump %ls: format %d", name, desc.Format);
    write_tga(config().dir + name, pixels.data(), width, height, width * 4);
}

// ---- stereo target discovery ----
bool plausible_pointer(uintptr_t p) {
    return p >= 0x10000 && p < 0x00007FFFFFFFFFFFull && (p & 7) == 0;
}

bool is_d3d_resource(uintptr_t p) {
    void* vtable = nullptr;
    return safe_get(reinterpret_cast<void*>(p), &vtable) &&
           std::find(g_resource_vtables.begin(), g_resource_vtables.end(), vtable) !=
               g_resource_vtables.end();
}

bool is_exe_object(uintptr_t p) {
    uintptr_t vtable = 0;
    return safe_get(reinterpret_cast<void*>(p), &vtable) && in_exe(vtable);
}

struct Found {
    TargetPath path;
    ID3D12Resource* resource;
};

// Walks engine objects (those with a vtable in the executable) from `object`, collecting the
// ID3D12Resources they reference and the field path that leads to each.
void walk(uintptr_t object, int depth, TargetPath& path, std::vector<Found>& found,
          std::vector<uintptr_t>& visited, int& budget) {
    if (--budget < 0 || std::find(visited.begin(), visited.end(), object) != visited.end()) {
        return;
    }
    visited.push_back(object);
    uintptr_t fields[0x40];
    int count = 0;
    for (; count < 0x40; count += 8) {
        if (!safe_read(reinterpret_cast<void*>(object + count * 8), fields + count, 8 * 8)) {
            break;
        }
    }
    for (int i = 0; i < count; ++i) {
        const uintptr_t q = fields[i];
        if (!plausible_pointer(q)) {
            continue;
        }
        path.offsets[depth] = static_cast<uint32_t>(i * 8);
        if (is_d3d_resource(q)) {
            TargetPath p = path;
            p.depth = depth + 1;
            found.push_back({p, reinterpret_cast<ID3D12Resource*>(q)});
        } else if (depth + 1 < 4 && is_exe_object(q)) {
            walk(q, depth + 1, path, found, visited, budget);
        }
    }
}

ID3D12Resource* follow(uintptr_t root, const TargetPath& path) {
    uintptr_t p = root;
    for (int i = 0; i < path.depth; ++i) {
        if (!safe_get(reinterpret_cast<void*>(p + path.offsets[i]), &p) || !plausible_pointer(p)) {
            return nullptr;
        }
    }
    return is_d3d_resource(p) ? reinterpret_cast<ID3D12Resource*>(p) : nullptr;
}

bool target_matches(ID3D12Resource* resource) {
    uint32_t w = 0, h = 0;
    stereo_eye_size(&w, &h);
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == w &&
           desc.Height == h;
}

std::string path_text(const TargetPath& path) {
    std::string s;
    char buf[16];
    for (int i = 0; i < path.depth; ++i) {
        snprintf(buf, sizeof(buf), "+0x%X", path.offsets[i]);
        s += (i ? " -> " : "") + std::string(buf);
    }
    return s;
}

ID3D12Resource* resolve_target(void* source) {
    if (g_resource_vtables.empty() || source == nullptr) {
        return nullptr;
    }
    const uintptr_t root = reinterpret_cast<uintptr_t>(source);
    if (g_target_path_valid) {
        ID3D12Resource* r = follow(root, g_target_path);
        if (r != nullptr && target_matches(r)) {
            return r;
        }
    }
    static DWORD last_search = 0;
    if (GetTickCount() - last_search < 1000) {
        return nullptr;
    }
    last_search = GetTickCount();
    std::vector<Found> found;
    std::vector<uintptr_t> visited;
    TargetPath path;
    int budget = 3000;
    walk(root, 0, path, found, visited, budget);
    for (const Found& f : found) {
        if (target_matches(f.resource)) {
            g_target_path = f.path;
            g_target_path_valid = true;
            const D3D12_RESOURCE_DESC d = f.resource->GetDesc();
            LOG("Stereo target found: %s (%llux%u format %d, %u slices, %u mips, flags 0x%X)",
                path_text(f.path).c_str(), d.Width, d.Height, d.Format, d.DepthOrArraySize,
                d.MipLevels, d.Flags);
            return f.resource;
        }
    }
    static int reports = 0;
    if (reports++ < 5) {
        uint32_t w = 0, h = 0;
        stereo_eye_size(&w, &h);
        LOG("Stereo target not found (want %ux%u, %zu resources, %zu objects visited):", w * 2, h,
            found.size(), visited.size());
        for (const Found& f : found) {
            const D3D12_RESOURCE_DESC d = f.resource->GetDesc();
            LOG("  %s: %llux%u format %d", path_text(f.path).c_str(), d.Width, d.Height, d.Format);
        }
    }
    return nullptr;
}

void collect_resource_vtables(ID3D12Resource* backbuffer0) {
    auto add = [](void* object) {
        void* vtable = *reinterpret_cast<void**>(object);
        if (std::find(g_resource_vtables.begin(), g_resource_vtables.end(), vtable) ==
            g_resource_vtables.end()) {
            g_resource_vtables.push_back(vtable);
        }
    };
    add(backbuffer0);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 64;
    desc.Height = 64;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> committed;
    if (SUCCEEDED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&committed)))) {
        add(committed.Get());
    }
    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = 1 << 20;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> placed_heap;
    ComPtr<ID3D12Resource> placed;
    if (SUCCEEDED(g_device->CreateHeap(&heap_desc, IID_PPV_ARGS(&placed_heap))) &&
        SUCCEEDED(g_device->CreatePlacedResource(placed_heap.Get(), 0, &desc,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&placed)))) {
        add(placed.Get());
    }
    LOG("%zu ID3D12Resource vtables known", g_resource_vtables.size());
}

// ---- Present ----
XrPosef g_last_screen_pose{{0, 0, 0, 1}, {0, 0, -2.2f}};
float g_last_screen_width = 2.6f;

void before_present() {
    RenderedFrame frame;
    bool have = false;
    {
        std::lock_guard lock(g_frames_mutex);
        if (!g_rendered.empty()) {
            frame = std::move(g_rendered.front());
            g_rendered.pop_front();
            have = true;
        }
    }
    const UINT index = g_swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = backbuffer(index);
    if (bb == nullptr) {
        return;
    }
    const auto eyes_state = static_cast<D3D12_RESOURCE_STATES>(config().eye_source_state);
    const bool stereo = have && frame.has_slot && frame.slot.stereo && frame.eyes[0] && frame.eyes[1];
    ID3D12Resource* eyes[2] = {stereo ? frame.eyes[0].Get() : nullptr,
                               stereo ? frame.eyes[1].Get() : nullptr};
    if (have && frame.has_slot && frame.slot.screen) {
        g_last_screen_pose = frame.slot.screen_pose;
        g_last_screen_width = frame.slot.screen_width;
    }

    if (xr::session_running()) {
        xr::Submission s;
        if (stereo) {
            const D3D12_RESOURCE_DESC d = eyes[0]->GetDesc();
            s.eyes[0] = eyes[0];
            s.eyes[1] = eyes[1];
            s.eyes_state = eyes_state;
            s.eye_width = static_cast<uint32_t>(d.Width);
            s.eye_height = d.Height;
            s.views[0] = frame.slot.views[0];
            s.views[1] = frame.slot.views[1];
        }
        const D3D12_RESOURCE_DESC bd = bb->GetDesc();
        s.ui = bb;
        s.ui_state = D3D12_RESOURCE_STATE_PRESENT;
        s.ui_width = static_cast<uint32_t>(bd.Width);
        s.ui_height = bd.Height;
        // Without a game frame (loading screens) the UI is shown on the virtual screen.
        const bool screen = !have || !frame.has_slot || frame.slot.screen || !stereo;
        if (screen) {
            xr::Quad q;
            q.pose = g_last_screen_pose;
            q.size = {g_last_screen_width, g_last_screen_width * bd.Height / static_cast<float>(bd.Width)};
            q.rect = {{0, 0}, {static_cast<int32_t>(bd.Width), static_cast<int32_t>(bd.Height)}};
            s.quads.push_back(q);
        } else {
            s.quads = frame.slot.hud;
        }
        xr::submit_frame(s);
    }

    if (g_dump_requested.exchange(false)) {
        if (stereo) {
            dump_resource(eyes[0], eyes_state, L"voidvr_left.tga");
            dump_resource(eyes[1], eyes_state, L"voidvr_right.tga");
        } else {
            LOG("Dump: no eye targets this frame");
        }
        dump_resource(bb, D3D12_RESOURCE_STATE_PRESENT, L"voidvr_ui.tga");
    }
    if (config().mirror && stereo) {
        mirror_left_eye(eyes[0], eyes_state, bb);
    }

    static DWORD stats_start = GetTickCount();
    static int stats_frames = 0, stats_stereo = 0;
    ++stats_frames;
    stats_stereo += stereo ? 1 : 0;
    if (const DWORD elapsed = GetTickCount() - stats_start; elapsed >= 10000) {
        LOG("%.1f fps (%d%% stereo), back buffer %ux%u", stats_frames * 1000.0f / elapsed,
            stats_stereo * 100 / stats_frames, static_cast<unsigned>(g_bb_width),
            static_cast<unsigned>(g_bb_height));
        stats_start = GetTickCount();
        stats_frames = stats_stereo = 0;
    }
}

bool our_swapchain(IDXGISwapChain* sc) {
    return g_swapchain != nullptr && static_cast<IDXGISwapChain*>(g_swapchain) == sc;
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!our_swapchain(sc) || (flags & DXGI_PRESENT_TEST) != 0) {
        return g_present(sc, sync, flags);
    }
    before_present();
    return g_present(sc, stereo_enabled() ? 0 : sync, flags);
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* params) {
    if (!our_swapchain(sc) || (flags & DXGI_PRESENT_TEST) != 0) {
        return g_present1(sc, sync, flags, params);
    }
    before_present();
    return g_present1(sc, stereo_enabled() ? 0 : sync, flags, params);
}

HRESULT STDMETHODCALLTYPE hook_resize(IDXGISwapChain* sc, UINT count, UINT width, UINT height,
                                      DXGI_FORMAT format, UINT flags) {
    if (our_swapchain(sc)) {
        release_backbuffers();
        LOG("ResizeBuffers %ux%u", width, height);
    }
    return g_resize(sc, count, width, height, format, flags);
}

HRESULT STDMETHODCALLTYPE hook_resize1(IDXGISwapChain3* sc, UINT count, UINT width, UINT height,
                                       DXGI_FORMAT format, UINT flags, const UINT* masks,
                                       IUnknown* const* queues) {
    if (our_swapchain(sc)) {
        release_backbuffers();
        LOG("ResizeBuffers1 %ux%u", width, height);
    }
    return g_resize1(sc, count, width, height, format, flags, masks, queues);
}

void hook_swapchain_methods(IDXGISwapChain3* sc) {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    void** vtable = *reinterpret_cast<void***>(sc);
    hook_function(vtable[8], reinterpret_cast<void*>(&hook_present),
                  reinterpret_cast<void**>(&g_present), "IDXGISwapChain::Present");
    hook_function(vtable[13], reinterpret_cast<void*>(&hook_resize),
                  reinterpret_cast<void**>(&g_resize), "IDXGISwapChain::ResizeBuffers");
    hook_function(vtable[22], reinterpret_cast<void*>(&hook_present1),
                  reinterpret_cast<void**>(&g_present1), "IDXGISwapChain1::Present1");
    hook_function(vtable[39], reinterpret_cast<void*>(&hook_resize1),
                  reinterpret_cast<void**>(&g_resize1), "IDXGISwapChain3::ResizeBuffers1");
}

void CALLBACK on_d3d_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                             D3D12_MESSAGE_ID id, LPCSTR description, void*) {
    if (severity > D3D12_MESSAGE_SEVERITY_WARNING) {
        return;
    }
    static std::atomic<int> count{0};
    const int n = ++count;
    if (n <= 300 || n % 500 == 0) {
        LOG("D3D12 %s #%d (%d): %s", severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "warning",
            id, n, description);
    }
}

// In VR the headset is the screen: the desktop window losing focus (Virtual Desktop, other
// apps) must not pause the game or release its input. Deactivation messages are swallowed.
WNDPROC g_game_wndproc = nullptr;

LRESULT CALLBACK game_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (stereo_enabled()) {
        switch (msg) {
            case WM_ACTIVATEAPP:
                if (wparam == FALSE) return 0;
                break;
            case WM_ACTIVATE:
                if (LOWORD(wparam) == WA_INACTIVE) return 0;
                break;
            case WM_NCACTIVATE:
                if (wparam == FALSE) return TRUE;
                break;
            case WM_KILLFOCUS:
                return 0;
            default:
                break;
        }
    }
    return CallWindowProcW(g_game_wndproc, hwnd, msg, wparam, lparam);
}

void keep_window_focus(HWND hwnd) {
    if (g_game_wndproc != nullptr || hwnd == nullptr) {
        return;
    }
    g_game_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&game_wndproc)));
    LOG(g_game_wndproc ? "Game window keeps focus in VR" : "WARNING: could not subclass the game window");
}

HRESULT STDMETHODCALLTYPE hook_create_swapchain(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
                                                const DXGI_SWAP_CHAIN_DESC1* desc,
                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT hr = g_create_swapchain(factory, device, hwnd, desc, fullscreen, output, out);
    if (FAILED(hr) || out == nullptr || *out == nullptr) {
        return hr;
    }
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) ||
        FAILED((*out)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        LOG("Swapchain %ux%u not on a D3D12 queue: ignored", desc->Width, desc->Height);
        return hr;
    }
    LOG("Game swapchain %ux%u format %d, %u buffers, flags 0x%X, swap effect %d", desc->Width,
        desc->Height, desc->Format, desc->BufferCount, desc->Flags, desc->SwapEffect);
    if (g_swapchain != nullptr) {
        release_backbuffers();
    }
    g_swapchain = sc3.Get();  // the engine owns it
    if (!g_queue) {
        g_queue = queue;
        queue->GetDevice(IID_PPV_ARGS(&g_device));
        if (!create_ring()) {
            LOG("ERROR: D3D12 command resources failed");
        }
        if (config().d3d_debug) {
            ComPtr<ID3D12InfoQueue1> info;
            DWORD cookie = 0;
            if (SUCCEEDED(g_device.As(&info)) &&
                SUCCEEDED(info->RegisterMessageCallback(&on_d3d_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                                        nullptr, &cookie))) {
                LOG("D3D12 debug messages go to this log");
            } else {
                LOG("D3D12 info queue unavailable");
            }
        }
        vr_on_swapchain(g_device.Get(), g_queue.Get());
    }
    keep_window_focus(hwnd);
    if (ID3D12Resource* bb0 = backbuffer(0)) {
        collect_resource_vtables(bb0);
    }
    hook_swapchain_methods(sc3.Get());
    return hr;
}

void hook_factory(IUnknown* factory) {
    static bool done = false;
    if (done || factory == nullptr) {
        return;
    }
    ComPtr<IDXGIFactory2> f2;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&f2)))) {
        return;
    }
    done = true;
    void** vtable = *reinterpret_cast<void***>(f2.Get());
    hook_function(vtable[15], reinterpret_cast<void*>(&hook_create_swapchain),
                  reinterpret_cast<void**>(&g_create_swapchain), "CreateSwapChainForHwnd");
}

void on_factory(void** out, HRESULT hr) {
    if (!g_engine_init_done.exchange(true)) {
        vr_engine_init();
    }
    if (SUCCEEDED(hr) && out != nullptr) {
        hook_factory(static_cast<IUnknown*>(*out));
    }
}

HRESULT WINAPI hook_create_factory1(REFIID riid, void** out) {
    const HRESULT hr = g_create_factory1(riid, out);
    on_factory(out, hr);
    return hr;
}

HRESULT WINAPI hook_create_factory2(UINT flags, REFIID riid, void** out) {
    const HRESULT hr = g_create_factory2(flags, riid, out);
    on_factory(out, hr);
    return hr;
}

}  // namespace

bool render_install() {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) {
        LOG("ERROR: dxgi.dll not loaded");
        return false;
    }
    const bool ok1 = hook_function(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")),
                                   reinterpret_cast<void*>(&hook_create_factory1),
                                   reinterpret_cast<void**>(&g_create_factory1), "CreateDXGIFactory1");
    const bool ok2 = hook_function(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")),
                                   reinterpret_cast<void*>(&hook_create_factory2),
                                   reinterpret_cast<void**>(&g_create_factory2), "CreateDXGIFactory2");
    return ok1 || ok2;
}

void frame_queue_push(const FrameSlot& slot) {
    std::lock_guard lock(g_frames_mutex);
    g_game_frames.push_back(slot);
    while (g_game_frames.size() > 4) {
        g_game_frames.pop_front();
    }
}

void render_on_frame_targets(void* left_root, void* right_root) {
    RenderedFrame frame;
    {
        std::lock_guard lock(g_frames_mutex);
        if (!g_game_frames.empty()) {
            frame.slot = std::move(g_game_frames.front());
            g_game_frames.pop_front();
            frame.has_slot = true;
        }
    }
    frame.eyes[0] = resolve_target(left_root);
    frame.eyes[1] = resolve_target(right_root);
    {
        std::lock_guard lock(g_frames_mutex);
        g_rendered.push_back(std::move(frame));
        while (g_rendered.size() > 4) {
            g_rendered.pop_front();
        }
    }
}

void render_enable_debug_layer() {
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto get_debug = d3d12 ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(
                                 GetProcAddress(d3d12, "D3D12GetDebugInterface"))
                           : nullptr;
    ComPtr<ID3D12Debug> debug;
    if (get_debug != nullptr && SUCCEEDED(get_debug(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        LOG("D3D12 debug layer enabled");
    } else {
        LOG("D3D12 debug layer unavailable");
    }
}

void render_request_dump() {
    g_dump_requested = true;
}

void render_backbuffer_size(uint32_t* width, uint32_t* height) {
    *width = g_bb_width;
    *height = g_bb_height;
}

}  // namespace vvr
