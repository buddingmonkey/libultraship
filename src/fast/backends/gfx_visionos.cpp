#ifdef __VISIONOS__

#include "fast/backends/gfx_visionos.h"

#include <Metal/Metal.hpp>
#include <SDL_events.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "fast/Fast3dGui.h"
#include "fast/backends/gfx_metal.h"
#include "fast/backends/gfx_xr_view.h"
#include "ship/Context.h"

namespace Fast {

namespace {
VisionOSRenderTarget gRenderTarget = { nullptr, nullptr, 0, 0 };
MTL::Texture* gGameTextures[2][2] = {};
int gWriteSlot = 0;
std::atomic<int> gReadySlot{ -1 };
VisionOSFrameHooks gFrameHooks = { nullptr, nullptr, nullptr, nullptr };
std::deque<VisionOSPointer> gPointerQueue;
std::mutex gPointerMutex;
struct PendingKey {
    int Scancode;
    bool Pressed;
};
std::deque<PendingKey> gKeyQueue;
std::mutex gKeyMutex;
struct PendingHoverRect {
    VisionOSHoverRect Rect;
    const ImGuiWindow* Window;
    float Area;
};
std::vector<PendingHoverRect> gPendingRects;
std::vector<VisionOSHoverRect> gHoverRects;
std::vector<VisionOSHoverRect> gPublishedRects;
std::mutex gRectMutex;

constexpr float kWindowDepthMax = 700.0f;
constexpr float kWindowDepthMin = 1.0f;
constexpr float kWindowDepthMargin = 0.9f;
constexpr float kWindowDepthRelease = 2.0f;
constexpr float kDioramaDepthDefault = 2.0f;
constexpr float kDioramaDepthMin = 0.5f;
constexpr float kDioramaDepthMax = 4.0f;
constexpr float kWindowSizeRange = 0.5f;
constexpr float kWindowRangeDefault = 1.3f;
constexpr float kWindowRangeMin = 0.5f;
constexpr float kWindowRangeMax = 4.0f;
constexpr float kWindowScaleDefault = kWindowRangeDefault / kWindowSizeRange;
constexpr float kWindowScaleMin = 0.5f;
constexpr float kWindowScaleMax = 8.0f;
constexpr uint32_t kRefreshRateDefault = 90;
constexpr float kTanHalfWidthDefault = 0.59f;

struct VisionOSEye {
    float X;
    float Y;
    float Z;
    bool Valid;
};
VisionOSEye gEyes[2] = {};
VisionOSWindow gShellWindow = {};
bool gShellWindowValid = false;
float gParallaxAcross = 0.0f;
float gParallaxRise = 0.0f;
float gWindowRange = kWindowRangeDefault;
float gWindowScale = kWindowScaleDefault;
float gDioramaDepth = kDioramaDepthDefault;
float gTanHalfWidth = kTanHalfWidthDefault;
float gTanHalfHeight = 0.0f;
float gSceneNear = std::numeric_limits<float>::max();
float gGlassDepth = kWindowDepthMax;
bool gFlatProjection = false;
int gViewIndex = 0;
uint32_t gViewCount = 1;
uint32_t gRefreshRate = kRefreshRateDefault;
XrViewGeometry gViewGeometry = {};
bool gViewGeometryValid = false;

void MoveGlass() {
    float target = gSceneNear * kWindowDepthMargin;
    if (target > kWindowDepthMax) {
        target = kWindowDepthMax;
    } else if (target < kWindowDepthMin) {
        target = kWindowDepthMin;
    }
    if (target < gGlassDepth) {
        gGlassDepth = target;
    } else {
        gGlassDepth +=
            (target - gGlassDepth) * (1.0f - expf(-1.0f / (static_cast<float>(gRefreshRate) * kWindowDepthRelease)));
    }
    gSceneNear = std::numeric_limits<float>::max();
}
float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

float TanHalfHeight() {
    if (gTanHalfHeight > 0.0f) {
        return gTanHalfHeight;
    }
    return gRenderTarget.Width > 0
               ? gTanHalfWidth * static_cast<float>(gRenderTarget.Height) / static_cast<float>(gRenderTarget.Width)
               : gTanHalfWidth;
}
} // namespace

void SetVisionOSParallaxReference(float across, float rise) {
    gParallaxAcross = across;
    gParallaxRise = rise;
}

void SetVisionOSWindow(float halfWidth, float halfHeight, float range) {
    gShellWindow = { halfWidth, halfHeight, range };
    gShellWindowValid = halfWidth > 0.0f && halfHeight > 0.0f && range > 0.0f;
}

VisionOSWindow GetVisionOSWindow() {
    if (gShellWindowValid) {
        return gShellWindow;
    }
    const float glass = 2.0f * kWindowSizeRange * gWindowScale;
    return { 0.5f * glass * gTanHalfWidth, 0.5f * glass * TanHalfHeight(), gWindowRange };
}

float GetVisionOSPictureAspect() {
    if (gTanHalfHeight <= 0.0f && gRenderTarget.Width == 0) {
        return 0.0f;
    }
    const float tanHalfHeight = TanHalfHeight();
    return tanHalfHeight > 0.0f ? gTanHalfWidth / tanHalfHeight : 0.0f;
}

void SetXrWindowDistance(float meters) {
    if (gShellWindowValid) {
        return;
    }
    gWindowRange = Clamp(meters, kWindowRangeMin, kWindowRangeMax);
}

float GetXrWindowDistance() {
    return gShellWindowValid ? gShellWindow.Range : gWindowRange;
}

void SetXrWindowScale(float scale) {
    if (gShellWindowValid) {
        return;
    }
    gWindowScale = Clamp(scale, kWindowScaleMin, kWindowScaleMax);
}

float GetXrWindowScale() {
    return gWindowScale;
}

void SetXrDioramaDepth(float meters) {
    gDioramaDepth = Clamp(meters, kDioramaDepthMin, kDioramaDepthMax);
}

void RecenterXrWindow() {
}

float GetXrWindowAngularWidth() {
    const VisionOSWindow window = GetVisionOSWindow();
    return window.Range > 0.0f ? 2.0f * atanf(window.HalfWidth / window.Range) : 0.0f;
}

void SetVisionOSEye(int view, float x, float y, float z) {
    if (view < 0 || view >= 2) {
        return;
    }
    gEyes[view] = { x, y, z, true };
}

bool GetXrViewGeometry(XrViewGeometry* geometry) {
    if (!gViewGeometryValid || gFlatProjection) {
        return false;
    }
    *geometry = gViewGeometry;
    return true;
}

void SetXrViewTangents(float tanHalfWidth, float tanHalfHeight) {
    gTanHalfWidth = tanHalfWidth;
    gTanHalfHeight = tanHalfHeight;
}

void SetXrSceneNear(float units) {
    if (units < gSceneNear) {
        gSceneNear = units;
    }
}

void SetXrFlatProjection(bool flat) {
    gFlatProjection = flat;
}

int GetXrViewIndex() {
    return gViewIndex;
}

void BeginVisionOSHoverRects() {
    gPendingRects.clear();
}

void EndVisionOSHoverRects() {
    gHoverRects.clear();
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr) {
        return;
    }

    std::stable_sort(gPendingRects.begin(), gPendingRects.end(),
                     [](const PendingHoverRect& a, const PendingHoverRect& b) { return a.Area > b.Area; });

    for (const ImGuiWindow* window : ctx->Windows) {
        if (!window->WasActive || window->Hidden || (window->Flags & ImGuiWindowFlags_NoMouseInputs) != 0) {
            continue;
        }

        const ImRect outer = window->OuterRectClipped;
        if (outer.GetWidth() > 0.0f && outer.GetHeight() > 0.0f) {
            VisionOSHoverRect blank{};
            blank.MinX = outer.Min.x;
            blank.MinY = outer.Min.y;
            blank.MaxX = outer.Max.x;
            blank.MaxY = outer.Max.y;
            gHoverRects.push_back(blank);
        }

        for (const PendingHoverRect& pending : gPendingRects) {
            if (pending.Window == window) {
                gHoverRects.push_back(pending.Rect);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(gRectMutex);
        gPublishedRects = gHoverRects;
    }
}

size_t CopyVisionOSHoverRects(VisionOSHoverRect* out, size_t max) {
    std::lock_guard<std::mutex> lock(gRectMutex);
    const size_t count = std::min(max, gPublishedRects.size());
    std::copy_n(gPublishedRects.begin(), count, out);
    return count;
}

void SetVisionOSFrameHooks(VisionOSFrameHooks hooks) {
    gFrameHooks = hooks;
}

void PushVisionOSPointer(VisionOSPointer pointer) {
    std::lock_guard<std::mutex> lock(gPointerMutex);
    if (!gPointerQueue.empty() && gPointerQueue.back().Pressed == pointer.Pressed) {
        gPointerQueue.back() = pointer;
        return;
    }
    gPointerQueue.push_back(pointer);
}

void PushVisionOSKey(int scancode, bool pressed) {
    std::lock_guard<std::mutex> lock(gKeyMutex);
    gKeyQueue.push_back({ scancode, pressed });
}

bool PeekVisionOSPointer(VisionOSPointer* pointer) {
    std::lock_guard<std::mutex> lock(gPointerMutex);
    if (gPointerQueue.empty()) {
        return false;
    }
    *pointer = gPointerQueue.front();
    return true;
}

void PopVisionOSPointer() {
    std::lock_guard<std::mutex> lock(gPointerMutex);
    if (!gPointerQueue.empty()) {
        gPointerQueue.pop_front();
    }
}

void SetVisionOSRenderTarget(void* device, void* commandQueue, uint32_t width, uint32_t height) {
    if (gRenderTarget.Width != width || gRenderTarget.Height != height) {
        gReadySlot.store(-1, std::memory_order_release);
        gWriteSlot = 0;
        for (MTL::Texture** eye : gGameTextures) {
            for (int slot = 0; slot < 2; ++slot) {
                if (eye[slot] != nullptr) {
                    eye[slot]->release();
                    eye[slot] = nullptr;
                }
            }
        }
    }
    gRenderTarget = { device, commandQueue, width, height };
}

void* GetVisionOSGameTexture(int eye) {
    if (eye < 0 || eye >= 2) {
        return nullptr;
    }
    if (gGameTextures[eye][gWriteSlot] != nullptr) {
        return gGameTextures[eye][gWriteSlot];
    }
    if (gRenderTarget.Device == nullptr || gRenderTarget.Width == 0 || gRenderTarget.Height == 0) {
        return nullptr;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatBGRA8Unorm, gRenderTarget.Width, gRenderTarget.Height, false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    gGameTextures[eye][gWriteSlot] = static_cast<MTL::Device*>(gRenderTarget.Device)->newTexture(descriptor);
    if (gGameTextures[eye][gWriteSlot] == nullptr) {
        SPDLOG_ERROR("visionOS: the game texture for eye {} was not made", eye);
    }
    return gGameTextures[eye][gWriteSlot];
}

void* GetVisionOSReadyGameTexture(int eye) {
    const int slot = gReadySlot.load(std::memory_order_acquire);
    if (eye < 0 || eye >= 2 || slot < 0) {
        return nullptr;
    }
    return gGameTextures[eye][slot];
}

void FlipVisionOSGameTextures() {
    gReadySlot.store(gWriteSlot, std::memory_order_release);
    gWriteSlot = 1 - gWriteSlot;
}

void ReportVisionOS(const char* text) {
    SPDLOG_INFO("visionOS: {}", text != nullptr ? text : "");
}

void SetVisionOSRefreshRate(uint32_t hz) {
    if (hz < 30 || hz > 240) {
        return;
    }
    gRefreshRate = hz;

    static uint32_t sReported = 0;
    const uint32_t moved = hz > sReported ? hz - sReported : sReported - hz;
    if (sReported != 0 && moved <= 2) {
        return;
    }
    sReported = hz;
    SPDLOG_INFO("visionOS: presenting at {} Hz", hz);
}

void SetVisionOSViewCount(uint32_t views) {
    gViewCount = views >= 2 ? 2 : 1;
}

GfxWindowBackendVisionOS::GfxWindowBackendVisionOS(GfxRenderingAPIMetal* renderingApi) : mRenderingApi(renderingApi) {
}

void GfxWindowBackendVisionOS::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                    uint32_t height, int32_t posX, int32_t posY) {
    mWidth = gRenderTarget.Width != 0 ? gRenderTarget.Width : width;
    mHeight = gRenderTarget.Height != 0 ? gRenderTarget.Height : height;
    mFullScreen = true;

    MTL::Texture* target = static_cast<MTL::Texture*>(GetVisionOSGameTexture(0));
    if (target == nullptr) {
        SPDLOG_ERROR("visionOS: the render target was not published before the window came up");
        return;
    }

    if (mRenderingApi == nullptr ||
        !mRenderingApi->MetalInitExternal(static_cast<MTL::Device*>(gRenderTarget.Device),
                                          static_cast<MTL::CommandQueue*>(gRenderTarget.CommandQueue), target)) {
        SPDLOG_ERROR("visionOS: the external Metal target did not come up");
        return;
    }

    GuiWindowInitData windowImpl;
    windowImpl.Backend = WindowBackend::FAST3D_VISIONOS_METAL;
    windowImpl.VisionOS.Width = mWidth;
    windowImpl.VisionOS.Height = mHeight;
    std::dynamic_pointer_cast<Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())->Init(windowImpl);
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetCurrentContext()->TestEngineHookItems = true;
    }
}

bool GfxWindowBackendVisionOS::OpenFrame() {
    if (gFrameHooks.OpenFrame == nullptr) {
        return false;
    }
    mFrameOpen = gFrameHooks.OpenFrame();
    return mFrameOpen;
}

uint32_t GfxWindowBackendVisionOS::BeginRenderFrame() {
    if (!mFrameOpen) {
        OpenFrame();
    }
    MoveGlass();
    mViewsThisFrame = gViewCount;
    return mViewsThisFrame;
}

void GfxWindowBackendVisionOS::BeginRenderView(uint32_t view) {
    gViewIndex = static_cast<int>(view);
    gViewGeometryValid = false;

    if (mRenderingApi != nullptr) {
        mRenderingApi->MetalSetExternalTarget(static_cast<MTL::Texture*>(GetVisionOSGameTexture(gViewIndex)));
    }

    const VisionOSWindow window = GetVisionOSWindow();
    if (view >= gViewCount || window.HalfWidth <= 0.0f || window.Range <= 0.0f || gTanHalfWidth <= 0.0f) {
        return;
    }

    const bool mono = gViewCount < 2 && gEyes[0].Valid && gEyes[1].Valid;
    if (!gEyes[view].Valid) {
        return;
    }
    const float eyeX = mono ? 0.5f * (gEyes[0].X + gEyes[1].X) : gEyes[view].X;
    const float eyeY = mono ? 0.5f * (gEyes[0].Y + gEyes[1].Y) : gEyes[view].Y;
    const float eyeZ = mono ? 0.5f * (gEyes[0].Z + gEyes[1].Z) : gEyes[view].Z;

    const float gain = gDioramaDepth / (window.Range + gDioramaDepth);
    const float acrossGlass = gain * gGlassDepth * gTanHalfWidth / window.HalfWidth;
    const float alongNormal = gGlassDepth / window.Range;

    gViewGeometry.eyeOffset[0] = (eyeX - gParallaxAcross) * acrossGlass;
    gViewGeometry.eyeOffset[1] = (eyeY - gParallaxRise) * acrossGlass;
    gViewGeometry.eyeOffset[2] = (eyeZ - window.Range) * alongNormal;
    gViewGeometry.windowDistance = gGlassDepth;
    gViewGeometryValid = true;
}

void GfxWindowBackendVisionOS::Close() {
    mIsRunning = false;
}

void GfxWindowBackendVisionOS::SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                                                    void (*onAllKeysUp)()) {
    mOnKeyDown = onKeyDown;
    mOnKeyUp = onKeyUp;
    mOnAllKeysUp = onAllKeysUp;
}

void GfxWindowBackendVisionOS::SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) {
    mOnMouseButtonDown = onMouseButtonDown;
    mOnMouseButtonUp = onMouseButtonUp;
}

void GfxWindowBackendVisionOS::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool isNowFullscreen)) {
    mOnFullscreenChanged = onFullscreenChanged;
}

void GfxWindowBackendVisionOS::SetFullscreen(bool fullscreen) {
}

void GfxWindowBackendVisionOS::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    *refreshRate = gRefreshRate;
}

void GfxWindowBackendVisionOS::SetCursorVisibility(bool visible) {
}

void GfxWindowBackendVisionOS::SetMousePos(int32_t posX, int32_t posY) {
}

void GfxWindowBackendVisionOS::GetMousePos(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendVisionOS::GetMouseDelta(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendVisionOS::GetMouseWheel(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

bool GfxWindowBackendVisionOS::GetMouseState(uint32_t btn) {
    return false;
}

void GfxWindowBackendVisionOS::SetMouseCapture(bool capture) {
}

bool GfxWindowBackendVisionOS::IsMouseCaptured() {
    return false;
}

void GfxWindowBackendVisionOS::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    *width = mWidth;
    *height = mHeight;
    *posX = 0;
    *posY = 0;
}

void GfxWindowBackendVisionOS::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
}

Ship::WindowRect GfxWindowBackendVisionOS::GetPrimaryMonitorRect() {
    return { 0, 0, static_cast<int32_t>(mWidth), static_cast<int32_t>(mHeight) };
}

void GfxWindowBackendVisionOS::HandleEvents() {
    if (gFrameHooks.PollState != nullptr) {
        gFrameHooks.PollState();
    }

    std::deque<PendingKey> keys;
    {
        std::lock_guard<std::mutex> lock(gKeyMutex);
        keys.swap(gKeyQueue);
    }
    if (!keys.empty()) {
        auto gui = std::dynamic_pointer_cast<Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui());
        if (gui != nullptr) {
            for (const PendingKey& key : keys) {
                WindowEvent event;
                event.VisionOS = { key.Scancode, key.Pressed };
                gui->HandleWindowEvents(event);
            }
        }
    }

    SDL_PumpEvents();

    // Nothing else empties this queue, so a queue that only grows makes SDL_PeepEvents slower each frame.
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_CONTROLLERDEVICEADDED - 1) > 0) {}
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMOVED + 1, SDL_LASTEVENT) > 0) {}
}

bool GfxWindowBackendVisionOS::IsFrameReady() {
    return true;
}

void GfxWindowBackendVisionOS::SwapBuffersBegin() {
    EndVisionOSHoverRects();

    if (!mFrameOpen) {
        if (!OpenFrame()) {
            return;
        }
        mViewsThisFrame = 1;
        gViewIndex = 0;
    }

    if (gViewIndex + 1 < static_cast<int>(mViewsThisFrame)) {
        return;
    }
    if (gFrameHooks.CloseFrame != nullptr) {
        gFrameHooks.CloseFrame();
    }
    mFrameOpen = false;
}

void GfxWindowBackendVisionOS::SwapBuffersEnd() {
}

double GfxWindowBackendVisionOS::GetTime() {
    return 0.0;
}

int GfxWindowBackendVisionOS::GetTargetFps() {
    return mTargetFps;
}

void GfxWindowBackendVisionOS::SetTargetFps(int fps) {
    mTargetFps = fps;
}

void GfxWindowBackendVisionOS::SetMaxFrameLatency(int latency) {
}

const char* GfxWindowBackendVisionOS::GetKeyName(int scancode) {
    return "";
}

bool GfxWindowBackendVisionOS::CanDisableVsync() {
    return false;
}

bool GfxWindowBackendVisionOS::IsRunning() {
    if (gFrameHooks.IsRunning != nullptr && !gFrameHooks.IsRunning()) {
        mIsRunning = false;
    }
    return mIsRunning;
}

void GfxWindowBackendVisionOS::Destroy() {
    mIsRunning = false;
}

bool GfxWindowBackendVisionOS::IsFullscreen() {
    return true;
}
} // namespace Fast

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* itemData) {
    if (id == 0 || bb.GetWidth() <= 0.0f || bb.GetHeight() <= 0.0f) {
        return;
    }
    if (itemData != nullptr && (itemData->ItemFlags & ImGuiItemFlags_Disabled) != 0) {
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (bb.GetWidth() >= display.x * 0.9f && bb.GetHeight() >= display.y * 0.9f) {
        return;
    }

    ImGuiWindow* window = ctx != nullptr ? ctx->CurrentWindow : nullptr;
    if (window == nullptr) {
        return;
    }

    if (id == window->ID || id == window->MoveId) {
        return;
    }

    if (ctx->WithinEndChildID != 0) {
        return;
    }

    ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    if (modal != nullptr && !ImGui::IsWindowWithinBeginStackOf(window, modal)) {
        return;
    }

    ImRect visible = bb;
    visible.ClipWith(window->ClipRect);
    if (visible.GetWidth() <= 0.0f || visible.GetHeight() <= 0.0f) {
        return;
    }

    Fast::PendingHoverRect pending{};
    pending.Rect.MinX = visible.Min.x;
    pending.Rect.MinY = visible.Min.y;
    pending.Rect.MaxX = visible.Max.x;
    pending.Rect.MaxY = visible.Max.y;
    pending.Rect.Identifier = id;
    pending.Window = window;
    pending.Area = visible.GetWidth() * visible.GetHeight();
    Fast::gPendingRects.push_back(pending);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
}

void ImGuiTestEngineHook_Log(ImGuiContext* ctx, const char* fmt, ...) {
}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext* ctx, ImGuiID id) {
    return nullptr;
}

#endif
