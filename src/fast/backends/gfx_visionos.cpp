#ifdef __VISIONOS__

#include "fast/backends/gfx_visionos.h"

#include <Metal/Metal.hpp>
#include <SDL_events.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <mutex>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "fast/Fast3dGui.h"
#include "fast/backends/gfx_metal.h"
#include "ship/Context.h"

namespace Fast {

namespace {
VisionOSCompositor gCompositor = { nullptr, nullptr, 0, 0 };
MTL::Texture* gGameTexture = nullptr;
VisionOSFrameHooks gFrameHooks = { nullptr, nullptr, nullptr };
VisionOSPointer gPointer{};
std::mutex gPointerMutex;
std::vector<VisionOSTrackingRect> gTrackingRects;
} // namespace

void BeginVisionOSTrackingRects() {
    gTrackingRects.clear();
}

void AddVisionOSTrackingRect(VisionOSTrackingRect rect) {
    gTrackingRects.push_back(rect);
}

size_t GetVisionOSTrackingRectCount() {
    return gTrackingRects.size();
}

VisionOSTrackingRect GetVisionOSTrackingRect(size_t index) {
    return gTrackingRects[index];
}

void SetVisionOSFrameHooks(VisionOSFrameHooks hooks) {
    gFrameHooks = hooks;
}

void SetVisionOSPointer(VisionOSPointer pointer) {
    std::lock_guard<std::mutex> lock(gPointerMutex);
    gPointer = pointer;
}

VisionOSPointer GetVisionOSPointer() {
    std::lock_guard<std::mutex> lock(gPointerMutex);
    return gPointer;
}

void SetVisionOSCompositor(void* device, void* commandQueue, uint32_t width, uint32_t height) {
    if (gGameTexture != nullptr && (gCompositor.Width != width || gCompositor.Height != height)) {
        gGameTexture->release();
        gGameTexture = nullptr;
    }
    gCompositor = { device, commandQueue, width, height };
}

VisionOSCompositor GetVisionOSCompositor() {
    return gCompositor;
}

void* GetVisionOSGameTexture() {
    if (gGameTexture != nullptr) {
        return gGameTexture;
    }
    if (gCompositor.Device == nullptr || gCompositor.Width == 0 || gCompositor.Height == 0) {
        return nullptr;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatBGRA8Unorm, gCompositor.Width, gCompositor.Height, false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    gGameTexture = static_cast<MTL::Device*>(gCompositor.Device)->newTexture(descriptor);
    if (gGameTexture == nullptr) {
        SPDLOG_ERROR("visionOS: the game texture was not made");
    }
    return gGameTexture;
}

GfxWindowBackendVisionOS::GfxWindowBackendVisionOS(GfxRenderingAPIMetal* renderingApi)
    : mRenderingApi(renderingApi) {
}

void GfxWindowBackendVisionOS::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                    uint32_t height, int32_t posX, int32_t posY) {
    mWidth = gCompositor.Width != 0 ? gCompositor.Width : width;
    mHeight = gCompositor.Height != 0 ? gCompositor.Height : height;
    mFullScreen = true;

    MTL::Texture* target = static_cast<MTL::Texture*>(GetVisionOSGameTexture());
    if (target == nullptr) {
        SPDLOG_ERROR("visionOS: the compositor was not published before the window came up");
        return;
    }

    // Interpreter::Init calls the rendering API's Init straight after this, and that reads the
    // device, so the external target has to be handed over here.
    if (mRenderingApi == nullptr ||
        !mRenderingApi->MetalInitExternal(static_cast<MTL::Device*>(gCompositor.Device),
                                          static_cast<MTL::CommandQueue*>(gCompositor.CommandQueue), target)) {
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
    return 1;
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

void GfxWindowBackendVisionOS::SetMouseCallbacks(bool (*onMouseButtonDown)(int btn),
                                                 bool (*onMouseButtonUp)(int btn)) {
    mOnMouseButtonDown = onMouseButtonDown;
    mOnMouseButtonUp = onMouseButtonUp;
}

void GfxWindowBackendVisionOS::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool isNowFullscreen)) {
    mOnFullscreenChanged = onFullscreenChanged;
}

void GfxWindowBackendVisionOS::SetFullscreen(bool fullscreen) {
}

void GfxWindowBackendVisionOS::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    *refreshRate = 90;
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
    // SDL has no video here, but the control deck still reads controller add and remove from
    // the same queue, and only a pump puts them there.
    SDL_PumpEvents();
}

bool GfxWindowBackendVisionOS::IsFrameReady() {
    return true;
}

void GfxWindowBackendVisionOS::SwapBuffersBegin() {
    // A GUI-only frame reaches here without BeginRenderFrame, the same way the OpenXR backend has
    // to open one late. Fast3D has already committed by now, so the shell makes its command buffer
    // after ours and the screen samples this frame, not the one before it.
    if (!mFrameOpen && !OpenFrame()) {
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

// ImGui calls these from ItemAdd when the test engine hooks are on. That is the only place which
// reports every item rectangle, and a tracking area needs one rectangle per item.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb,
                                 const ImGuiLastItemData* itemData) {
    // An item with no ID cannot be interacted with; plain text is the common case.
    if (id == 0 || bb.GetWidth() <= 0.0f || bb.GetHeight() <= 0.0f) {
        return;
    }
    if (itemData != nullptr && (itemData->ItemFlags & ImGuiItemFlags_Disabled) != 0) {
        return;
    }

    // ImGui adds the window and the dock space as items too, and it adds them after their content.
    // A full-size item would paint over every button in the mask, so leave those out. What is left
    // keeps its submission order, which puts an inner item over the one that contains it.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (bb.GetWidth() >= display.x * 0.9f && bb.GetHeight() >= display.y * 0.9f) {
        return;
    }

    ImGuiWindow* window = ctx != nullptr ? ctx->CurrentWindow : nullptr;
    if (window == nullptr) {
        return;
    }

    // The window itself and its title bar come through ItemAdd the same way a button does. Neither
    // is something to press, and the window covers everything inside it.
    if (id == window->ID || id == window->MoveId) {
        return;
    }

    // A modal takes every click, so nothing behind it can be pressed. This is the rule ImGui uses
    // itself, and without it the implicit debug window offers items under the dimmed screen.
    ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    if (modal != nullptr && !ImGui::IsWindowWithinBeginStackOf(window, modal)) {
        return;
    }

    Fast::VisionOSTrackingRect rect{};
    rect.MinX = bb.Min.x;
    rect.MinY = bb.Min.y;
    rect.MaxX = bb.Max.x;
    rect.MaxY = bb.Max.y;
    rect.Identifier = id;
    Fast::AddVisionOSTrackingRect(rect);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
}

void ImGuiTestEngineHook_Log(ImGuiContext* ctx, const char* fmt, ...) {
}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext* ctx, ImGuiID id) {
    return nullptr;
}

#endif
