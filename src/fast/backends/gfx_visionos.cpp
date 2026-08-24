#ifdef __VISIONOS__

#include "fast/backends/gfx_visionos.h"

#include <Metal/Metal.hpp>
#include <SDL_events.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <deque>
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
VisionOSFrameHooks gFrameHooks = { nullptr, nullptr, nullptr, nullptr };
std::deque<VisionOSPointer> gPointerQueue;
std::mutex gPointerMutex;
// An item as ImGui reported it, with what the mask needs to put it in order later.
struct PendingTrackingRect {
    VisionOSTrackingRect Rect;
    const ImGuiWindow* Window;
    float Area;
};
std::vector<PendingTrackingRect> gPendingRects;
std::vector<VisionOSTrackingRect> gTrackingRects;
} // namespace

void BeginVisionOSTrackingRects() {
    gPendingRects.clear();
}

void EndVisionOSTrackingRects() {
    gTrackingRects.clear();
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr) {
        return;
    }

    // The mask has to answer what ImGui answers: which window is in front here, then which item in
    // that window. Order of submission decides neither, so a container that ImGui reports after its
    // content cannot cover it, and a future widget cannot bring the same fault back.
    std::stable_sort(gPendingRects.begin(), gPendingRects.end(),
                     [](const PendingTrackingRect& a, const PendingTrackingRect& b) { return a.Area > b.Area; });

    // ctx->Windows is back to front, with a child after its parent, which is the order the mask
    // needs. ImGui skips the same windows in FindHoveredWindowEx.
    for (const ImGuiWindow* window : ctx->Windows) {
        if (!window->WasActive || window->Hidden || (window->Flags & ImGuiWindowFlags_NoMouseInputs) != 0) {
            continue;
        }

        // A window takes the hover from everything behind it, so it hides it in the mask as well.
        const ImRect outer = window->OuterRectClipped;
        if (outer.GetWidth() > 0.0f && outer.GetHeight() > 0.0f) {
            VisionOSTrackingRect blank{};
            blank.MinX = outer.Min.x;
            blank.MinY = outer.Min.y;
            blank.MaxX = outer.Max.x;
            blank.MaxY = outer.Max.y;
            gTrackingRects.push_back(blank);
        }

        // The largest item goes down first, so the smallest item that holds a point wins it.
        for (const PendingTrackingRect& pending : gPendingRects) {
            if (pending.Window == window) {
                gTrackingRects.push_back(pending.Rect);
            }
        }
    }
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

void PushVisionOSPointer(VisionOSPointer pointer) {
    std::lock_guard<std::mutex> lock(gPointerMutex);
    // Only a change of the button needs its own step. Anything else is the same press moving, so
    // keep the newest place and hold the queue to the two steps a pinch really has.
    if (!gPointerQueue.empty() && gPointerQueue.back().Pressed == pointer.Pressed) {
        gPointerQueue.back() = pointer;
        return;
    }
    gPointerQueue.push_back(pointer);
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
    // The main loop calls this every pass, including the passes it spends off screen, so this is
    // where the shell can report a pause, a resume or an invalidated layer.
    if (gFrameHooks.PollState != nullptr) {
        gFrameHooks.PollState();
    }

    // SDL has no video here, but the control deck still reads controller add and remove from
    // the same queue, and only a pump puts them there.
    SDL_PumpEvents();
}

bool GfxWindowBackendVisionOS::IsFrameReady() {
    return true;
}

void GfxWindowBackendVisionOS::SwapBuffersBegin() {
    // ImGui has ended its frame by now, so the window order is settled and the mask can be put in
    // order. The shell rasterizes it inside CloseFrame.
    EndVisionOSTrackingRects();

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
// reports every item rectangle, and a tracking area needs one rectangle per item. The hook only
// collects; EndVisionOSTrackingRects puts the rectangles in order.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb,
                                 const ImGuiLastItemData* itemData) {
    // An item with no ID cannot be interacted with; plain text is the common case.
    if (id == 0 || bb.GetWidth() <= 0.0f || bb.GetHeight() <= 0.0f) {
        return;
    }
    if (itemData != nullptr && (itemData->ItemFlags & ImGuiItemFlags_Disabled) != 0) {
        return;
    }

    // A full-size item, such as a dock space, is a place to put content, not a place to press. It
    // would give the whole screen one gaze highlight.
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

    // EndChild reports the whole child window as one item of the parent. ImGui never hovers it,
    // because the child is the window in front there, so it is not a place to press either.
    if (ctx->WithinEndChildID != 0) {
        return;
    }

    // A modal takes every click, so nothing behind it can be pressed. This is the rule ImGui uses
    // itself, and without it the implicit debug window offers items under the dimmed screen.
    ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    if (modal != nullptr && !ImGui::IsWindowWithinBeginStackOf(window, modal)) {
        return;
    }

    // ItemAdd calls this hook before it tests the clip rectangle, so an item scrolled out of view
    // still arrives, at its full size.
    ImRect visible = bb;
    visible.ClipWith(window->ClipRect);
    if (visible.GetWidth() <= 0.0f || visible.GetHeight() <= 0.0f) {
        return;
    }

    Fast::PendingTrackingRect pending{};
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
