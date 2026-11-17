#include "xr.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "config.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace vvr::xr {

namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
std::atomic<bool> g_running{false};
std::atomic<bool> g_recenter_requested{false};
LUID g_luid{};
bool g_have_luid = false;
XrViewConfigurationView g_view_config[2]{};
PFN_xrConvertWin32PerformanceCounterToTimeKHR g_qpc_to_time = nullptr;
std::atomic<XrTime> g_last_display_time{0};
std::atomic<XrDuration> g_period{11111111};

// Serialises session begin/end (game thread) against frame submission (presenting thread).
std::mutex g_session_mutex;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
std::vector<int64_t> g_formats;

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t format = 0;
};
Swapchain g_eyes;
Swapchain g_ui;

constexpr int kFramesInFlight = 3;
ComPtr<ID3D12CommandAllocator> g_allocators[kFramesInFlight];
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
uint64_t g_fence_values[kFramesInFlight]{};
uint64_t g_fence_next = 1;
HANDLE g_fence_event = nullptr;
int g_allocator_index = 0;

bool check(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        char text[XR_MAX_RESULT_STRING_SIZE] = "?";
        if (g_instance != XR_NULL_HANDLE) {
            xrResultToString(g_instance, result, text);
        }
        LOG("OpenXR %s failed: %s (%d)", what, text, result);
        return false;
    }
    return true;
}

// Swapchain format holding the source's bytes unchanged. The game writes sRGB-encoded values
// into UNORM targets, so the *_SRGB variant makes the runtime decode them correctly.
int64_t format_for(DXGI_FORMAT source) {
    std::vector<DXGI_FORMAT> wanted;
    switch (source) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            wanted = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM};
            break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            wanted = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
            break;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            wanted = {DXGI_FORMAT_R10G10B10A2_UNORM};
            break;
        default:
            break;
    }
    for (DXGI_FORMAT f : wanted) {
        if (std::find(g_formats.begin(), g_formats.end(), static_cast<int64_t>(f)) != g_formats.end()) {
            return f;
        }
    }
    return 0;
}

bool ensure_swapchain(Swapchain& sc, uint32_t width, uint32_t height, DXGI_FORMAT source,
                      const char* name) {
    const int64_t format = format_for(source);
    if (format == 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG("No swapchain format can hold DXGI format %d", source);
        }
        return false;
    }
    if (sc.handle != XR_NULL_HANDLE && sc.width == width && sc.height == height &&
        sc.format == format) {
        return true;
    }
    if (sc.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(sc.handle);
        sc = Swapchain{};
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!check(xrCreateSwapchain(g_session, &info, &sc.handle), "xrCreateSwapchain")) {
        sc.handle = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
    sc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(sc.handle, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()));
    sc.width = width;
    sc.height = height;
    sc.format = format;
    LOG("Created %s swapchain %ux%u format %lld (%u images)", name, width, height, format, count);
    return true;
}

bool create_copy_resources() {
    for (auto& allocator : g_allocators) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&allocator)))) {
            return false;
        }
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_list)))) {
        return false;
    }
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        return false;
    }
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

void begin_copies() {
    const int index = g_allocator_index;
    if (g_fence->GetCompletedValue() < g_fence_values[index]) {
        g_fence->SetEventOnCompletion(g_fence_values[index], g_fence_event);
        WaitForSingleObject(g_fence_event, 1000);
    }
    g_allocators[index]->Reset();
    g_list->Reset(g_allocators[index].Get(), nullptr);
}

void submit_copies() {
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    const int index = g_allocator_index;
    g_allocator_index = (g_allocator_index + 1) % kFramesInFlight;
    g_fence_values[index] = g_fence_next++;
    g_queue->Signal(g_fence.Get(), g_fence_values[index]);
}

void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    g_list->ResourceBarrier(1, &barrier);
}

// Acquires an image of `sc` and records the copy of each source (width x height at its origin)
// side by side: source i lands at x = i * width.
bool copy_into(Swapchain& sc, ID3D12Resource* const* sources, int count, D3D12_RESOURCE_STATES state,
               uint32_t width, uint32_t height) {
    uint32_t image = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!check(xrAcquireSwapchainImage(sc.handle, &acquire, &image), "xrAcquireSwapchainImage")) {
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100000000;  // 100 ms
    if (!check(xrWaitSwapchainImage(sc.handle, &wait), "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc.handle, &release);
        return false;
    }
    ID3D12Resource* target = sc.images[image].texture;
    // Runtimes hand colour images out in RENDER_TARGET state and expect them back in it.
    transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    for (int i = 0; i < count; ++i) {
        ID3D12Resource* source = sources[i];
        transition(source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = target;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = source;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, width, height, 1};
        g_list->CopyTextureRegion(&dst, i * width, 0, 0, &src, &box);
        transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
    transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return true;
}

void release_image(Swapchain& sc) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    check(xrReleaseSwapchainImage(sc.handle, &release), "xrReleaseSwapchainImage");
}

// ---- the waypoint pin (a static image drawn once) ----
Swapchain g_marker;
ComPtr<ID3D12Resource> g_marker_upload;
bool g_marker_ready = false;
bool g_marker_failed = false;

// A lime teardrop pointing down with a dark outline and a dark centre, supersampled edges.
// Colours are sRGB values (the swapchain is an sRGB format when available).
std::vector<uint8_t> marker_pixels(bool bgra) {
    const int n = kMarkerSize;
    std::vector<uint8_t> px(static_cast<size_t>(n) * n * 4, 0);
    const double cx = n * 0.5, cy = n * 0.38, r = n * 0.30, tip = n * 0.96;
    // 0 = outside, 1 = outline, 2 = fill, 3 = centre hole
    auto classify = [&](double x, double y) {
        auto inside = [&](double grow) {
            const double dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= (r + grow) * (r + grow)) return true;
            if (y > cy && y < tip + grow) {
                const double half = (r + grow) * (tip + grow - y) / (tip + grow - cy);
                return std::fabs(dx) <= half;
            }
            return false;
        };
        if (!inside(0.0)) return 0;
        if (!inside(-n * 0.05)) return 1;
        const double dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= (r * 0.40) * (r * 0.40)) return 3;
        return 2;
    };
    const double colours[4][4] = {{0, 0, 0, 0}, {18, 22, 12, 235}, {205, 245, 50, 255}, {18, 22, 12, 235}};
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            double acc[4] = {};
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const int c = classify(x + (sx + 0.5) / 4.0, y + (sy + 0.5) / 4.0);
                    const double a = colours[c][3];
                    for (int k = 0; k < 3; ++k) acc[k] += colours[c][k] * a;
                    acc[3] += a;
                }
            }
            uint8_t* p = &px[(static_cast<size_t>(y) * n + x) * 4];
            const double alpha = acc[3] / 16.0;
            for (int k = 0; k < 3; ++k) {
                // Premultiplied alpha, like the UI image (the layer is blended as premultiplied).
                const double v = acc[3] > 0 ? acc[k] / acc[3] * (alpha / 255.0) : 0.0;
                p[bgra ? 2 - k : k] = static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
            }
            p[3] = static_cast<uint8_t>(std::clamp(alpha, 0.0, 255.0));
        }
    }
    return px;
}

bool ensure_marker() {
    if (g_marker_ready || g_marker_failed) {
        return g_marker_ready;
    }
    g_marker_failed = true;  // until it succeeds
    bool bgra = false;
    int64_t format = format_for(DXGI_FORMAT_R8G8B8A8_UNORM);
    if (format == 0) {
        format = format_for(DXGI_FORMAT_B8G8R8A8_UNORM);
        bgra = true;
    }
    if (format == 0) {
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = kMarkerSize;
    info.height = kMarkerSize;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!check(xrCreateSwapchain(g_session, &info, &g_marker.handle), "xrCreateSwapchain (marker)")) {
        g_marker.handle = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(g_marker.handle, 0, &count, nullptr);
    g_marker.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_marker.handle, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(g_marker.images.data()));
    // Upload buffer: rows of kMarkerSize * 4 bytes (512, already 256-aligned).
    const UINT pitch = kMarkerSize * 4;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(pitch) * kMarkerSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&g_marker_upload)))) {
        return false;
    }
    const std::vector<uint8_t> pixels = marker_pixels(bgra);
    void* mapped = nullptr;
    if (FAILED(g_marker_upload->Map(0, nullptr, &mapped))) {
        return false;
    }
    memcpy(mapped, pixels.data(), pixels.size());
    g_marker_upload->Unmap(0, nullptr);

    uint32_t image = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100000000;
    if (!check(xrAcquireSwapchainImage(g_marker.handle, &acquire, &image), "xrAcquireSwapchainImage (marker)") ||
        !check(xrWaitSwapchainImage(g_marker.handle, &wait), "xrWaitSwapchainImage (marker)")) {
        return false;
    }
    ID3D12Resource* target = g_marker.images[image].texture;
    begin_copies();
    transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_marker_upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(format);
    src.PlacedFootprint.Footprint.Width = kMarkerSize;
    src.PlacedFootprint.Footprint.Height = kMarkerSize;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    submit_copies();
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    check(xrReleaseSwapchainImage(g_marker.handle, &release), "xrReleaseSwapchainImage (marker)");
    g_marker_ready = true;
    g_marker_failed = false;
    LOG("Created the waypoint marker image");
    return true;
}

}  // namespace

bool init_instance() {
    if (g_instance != XR_NULL_HANDLE) {
        return true;
    }
    uint32_t ext_count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
    std::vector<XrExtensionProperties> props(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, props.data());
    auto has = [&](const char* name) {
        return std::any_of(props.begin(), props.end(),
                           [&](const XrExtensionProperties& p) { return strcmp(p.extensionName, name) == 0; });
    };
    if (!has(XR_KHR_D3D12_ENABLE_EXTENSION_NAME)) {
        LOG("OpenXR runtime without D3D12 support (or no runtime)");
        return false;
    }
    std::vector<const char*> extensions = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    const bool qpc = has(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    if (qpc) {
        extensions.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    }
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(info.applicationInfo.applicationName, "VOID/BREAKER VR");
    info.applicationInfo.applicationVersion = 1;
    strcpy_s(info.applicationInfo.engineName, "Unreal Engine 5");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.enabledExtensionNames = extensions.data();
    if (!check(xrCreateInstance(&info, &g_instance), "xrCreateInstance")) {
        g_instance = XR_NULL_HANDLE;
        return false;
    }
    XrInstanceProperties instance_props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &instance_props);
    LOG("OpenXR runtime: %s %u.%u.%u", instance_props.runtimeName,
        XR_VERSION_MAJOR(instance_props.runtimeVersion), XR_VERSION_MINOR(instance_props.runtimeVersion),
        XR_VERSION_PATCH(instance_props.runtimeVersion));
    if (qpc) {
        xrGetInstanceProcAddr(g_instance, "xrConvertWin32PerformanceCounterToTimeKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&g_qpc_to_time));
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(xrGetSystem(g_instance, &system_info, &g_system), "xrGetSystem")) {
        xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties system_props{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, g_system, &system_props);
    LOG("Headset: %s (max %u layers)", system_props.systemName,
        system_props.graphicsProperties.maxLayerCount);

    PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (get_requirements != nullptr &&
        check(get_requirements(g_instance, g_system, &requirements), "D3D12 requirements")) {
        g_luid = requirements.adapterLuid;
        g_have_luid = true;
    }

    uint32_t count = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (count != 2) {
        LOG("Unexpected view count %u", count);
        return false;
    }
    g_view_config[0].type = g_view_config[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(g_instance, g_system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count,
                                      g_view_config);
    LOG("Recommended eye size %ux%u", g_view_config[0].recommendedImageRectWidth,
        g_view_config[0].recommendedImageRectHeight);
    return true;
}

bool available() {
    return g_instance != XR_NULL_HANDLE;
}

bool adapter_luid(LUID* out) {
    if (!g_have_luid) {
        return false;
    }
    *out = g_luid;
    return true;
}

bool create_session(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (g_instance == XR_NULL_HANDLE) {
        return false;
    }
    if (g_session != XR_NULL_HANDLE) {
        return true;
    }
    g_device = device;
    g_queue = queue;
    if (!create_copy_resources()) {
        LOG("Failed to create D3D12 copy resources");
        return false;
    }
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device;
    binding.queue = queue;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = g_system;
    if (!check(xrCreateSession(g_instance, &info, &g_session), "xrCreateSession")) {
        g_session = XR_NULL_HANDLE;
        return false;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1.0f;
    if (!check(xrCreateReferenceSpace(g_session, &space, &g_local), "xrCreateReferenceSpace")) {
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
    g_formats.resize(count);
    xrEnumerateSwapchainFormats(g_session, count, &count, g_formats.data());
    LOG("OpenXR session created (%u swapchain formats)", count);
    input_init(g_instance, g_session, g_local);
    return true;
}

bool session_running() {
    return g_running;
}

bool consume_recenter_request() {
    return g_recenter_requested.exchange(false);
}

uint32_t recommended_width() {
    return g_view_config[0].recommendedImageRectWidth;
}

uint32_t recommended_height() {
    return g_view_config[0].recommendedImageRectHeight;
}

void poll_events() {
    if (g_instance == XR_NULL_HANDLE) {
        return;
    }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            LOG("OpenXR session state %d", changed.state);
            std::lock_guard lock(g_session_mutex);
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_running = check(xrBeginSession(g_session, &begin), "xrBeginSession");
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                g_running = false;
                check(xrEndSession(g_session), "xrEndSession");
            } else if (changed.state == XR_SESSION_STATE_EXITING ||
                       changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOG("OpenXR instance loss pending");
            g_running = false;
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // The user recentred from the headset (e.g. holding the Meta button).
            g_recenter_requested = true;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

XrTime predicted_time() {
    const XrTime last = g_last_display_time;
    const XrDuration period = g_period;
    const int frames = config().predict_frames;
    XrTime now = 0;
    if (g_qpc_to_time != nullptr) {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        g_qpc_to_time(g_instance, &qpc, &now);
    }
    if (now == 0) {
        return last != 0 ? last + period * frames : 0;
    }
    if (last == 0) {
        return now + period * (frames + 1);
    }
    // The frame shown now was predicted for `last`; this one is `frames` behind it. Clamp so
    // stalls never predict into the past nor too far ahead.
    return std::clamp<XrTime>(last + period * frames, now + period / 2, now + period * (frames + 2));
}

bool locate_views(XrTime time, XrView views[2]) {
    if (g_session == XR_NULL_HANDLE || !g_running || time == 0) {
        return false;
    }
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = g_local;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    views[0].type = views[1].type = XR_TYPE_VIEW;
    if (!check(xrLocateViews(g_session, &locate, &view_state, 2, &count, views), "xrLocateViews")) {
        return false;
    }
    return (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
}

bool submit_frame(const Submission& s) {
    std::lock_guard lock(g_session_mutex);
    if (!g_running) {
        return false;
    }
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (!check(xrWaitFrame(g_session, nullptr, &state), "xrWaitFrame")) {
        return false;
    }
    g_last_display_time = state.predictedDisplayTime;
    g_period = state.predictedDisplayPeriod;
    if (!check(xrBeginFrame(g_session, nullptr), "xrBeginFrame")) {
        return false;
    }

    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2]{};
    std::vector<XrCompositionLayerQuad> quads;

    if (state.shouldRender) {
        bool eyes = false, ui = false;
        if (s.eyes[0] != nullptr && s.eyes[1] != nullptr && s.eye_width > 0) {
            eyes = ensure_swapchain(g_eyes, s.eye_width * 2, s.eye_height, s.eyes[0]->GetDesc().Format,
                                    "eye");
        }
        if (s.ui != nullptr && !s.quads.empty()) {
            ui = ensure_swapchain(g_ui, s.ui_width, s.ui_height, s.ui->GetDesc().Format, "UI");
        }
        if (eyes || ui) {
            begin_copies();
            eyes = eyes && copy_into(g_eyes, s.eyes, 2, s.eyes_state, s.eye_width, s.eye_height);
            ui = ui && copy_into(g_ui, &s.ui, 1, s.ui_state, s.ui_width, s.ui_height);
            submit_copies();
            if (eyes) release_image(g_eyes);
            if (ui) release_image(g_ui);
        }
        if (eyes) {
            for (int eye = 0; eye < 2; ++eye) {
                views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                views[eye].pose = s.views[eye].pose;
                views[eye].fov = s.views[eye].fov;
                views[eye].subImage.swapchain = g_eyes.handle;
                views[eye].subImage.imageRect.offset = {static_cast<int32_t>(eye * s.eye_width), 0};
                views[eye].subImage.imageRect.extent = {static_cast<int32_t>(s.eye_width),
                                                        static_cast<int32_t>(s.eye_height)};
            }
            projection.space = g_local;
            projection.viewCount = 2;
            projection.views = views;
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
        }
        const bool markers = std::any_of(s.quads.begin(), s.quads.end(), [](const Quad& q) { return q.marker; }) &&
                             ensure_marker();
        if (ui || markers) {
            quads.reserve(s.quads.size());
            // Runtimes allow few layers (16 is common, the projection counts as one).
            constexpr size_t kMaxQuads = 15;
            for (const Quad& q : s.quads) {
                if (quads.size() >= kMaxQuads) {
                    break;
                }
                if (q.marker ? !markers : !ui) {
                    continue;
                }
                XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
                quad.space = g_local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = q.marker ? g_marker.handle : g_ui.handle;
                quad.subImage.imageRect = q.rect;
                quad.pose = q.pose;
                quad.size = q.size;
                quads.push_back(quad);
            }
            for (const auto& quad : quads) {
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
            }
        }
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = static_cast<uint32_t>(layers.size());
    end.layers = layers.empty() ? nullptr : layers.data();
    return check(xrEndFrame(g_session, &end), "xrEndFrame");
}

}  // namespace vvr::xr
