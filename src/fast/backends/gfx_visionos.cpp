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
VisionOSCompositor gCompositor = { nullptr, nullptr, 0, 0 };
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
// An item as ImGui reported it, with what the mask needs to put it in order later.
struct PendingTrackingRect {
    VisionOSTrackingRect Rect;
    const ImGuiWindow* Window;
    float Area;
};
std::vector<PendingTrackingRect> gPendingRects;
std::vector<VisionOSTrackingRect> gTrackingRects;
std::vector<VisionOSTrackingRect> gPublishedRects;
std::mutex gRectMutex;

// The window in the room, in the units gfx_xr_view.h asks for. The model is the one the OpenXR
// backend uses; only the window differs, because this one is a fixed quad and not an angular size
// the player can resize. See xr-window-depth-model before changing the gain.
constexpr float kWindowDepthMax = 700.0f;
constexpr float kWindowDepthMin = 1.0f;
constexpr float kWindowDepthMargin = 0.9f;
constexpr float kWindowDepthRelease = 2.0f;
constexpr float kDioramaDepthDefault = 2.0f;
constexpr float kDioramaDepthMin = 0.5f;
constexpr float kDioramaDepthMax = 4.0f;
// Meters of glass at scale 1, against the tangents the game asks for, and how far the window hangs
// from the viewer. Both are the numbers the OpenXR backend works in, so the menu sliders mean the
// same thing on both headsets.
constexpr float kWindowSizeRange = 0.5f;
constexpr float kWindowRangeDefault = 1.3f;
constexpr float kWindowRangeMin = 0.5f;
constexpr float kWindowRangeMax = 4.0f;
constexpr float kWindowScaleDefault = kWindowRangeDefault / kWindowSizeRange;
constexpr float kWindowScaleMin = 0.5f;
constexpr float kWindowScaleMax = 8.0f;
// A window placed with the head bowed hangs low but stands up, and only a steep look tips it.
constexpr float kRiseFlat = 0.35f;
constexpr float kRiseMax = 1.22f;
constexpr uint32_t kRefreshRateDefault = 90;
// The half-angle the window covers across, before the game has loaded a projection to ask for its
// own. About 61 degrees, which is what Banjo-Kazooie asks for once it runs. ApplyXrProjection only
// reports the tangents on a frame it already had a window for, so without a value to start from
// the two would wait on each other forever.
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
bool gRecenterWanted = false;
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

// In at once, so nothing is ever left standing in front of the window; out slowly, so one near
// object in one frame does not throw the world back and hold it there.
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

// Before the game loads a projection there is only the shape of the picture to go on.
float TanHalfHeight() {
    if (gTanHalfHeight > 0.0f) {
        return gTanHalfHeight;
    }
    return gCompositor.Width > 0
               ? gTanHalfWidth * static_cast<float>(gCompositor.Height) / static_cast<float>(gCompositor.Width)
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
    const float tanHalfHeight = TanHalfHeight();
    return tanHalfHeight > 0.0f ? gTanHalfWidth / tanHalfHeight : 1.0f;
}

bool TakeVisionOSRecenter() {
    const bool wanted = gRecenterWanted;
    gRecenterWanted = false;
    return wanted;
}

// A shell that reports its own window owns the placement, and the system owns it there. The menu
// keeps only what belongs to the app.
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
    gRecenterWanted = true;
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

    {
        std::lock_guard<std::mutex> lock(gRectMutex);
        gPublishedRects = gTrackingRects;
    }
}

size_t GetVisionOSTrackingRectCount() {
    return gTrackingRects.size();
}

VisionOSTrackingRect GetVisionOSTrackingRect(size_t index) {
    return gTrackingRects[index];
}

size_t CopyVisionOSTrackingRects(VisionOSTrackingRect* out, size_t max) {
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
    // Only a change of the button needs its own step. Anything else is the same press moving, so
    // keep the newest place and hold the queue to the two steps a pinch really has.
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

void SetVisionOSCompositor(void* device, void* commandQueue, uint32_t width, uint32_t height) {
    if (gCompositor.Width != width || gCompositor.Height != height) {
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
    gCompositor = { device, commandQueue, width, height };
}

VisionOSCompositor GetVisionOSCompositor() {
    return gCompositor;
}

void* GetVisionOSGameTexture(int eye) {
    if (eye < 0 || eye >= 2) {
        return nullptr;
    }
    if (gGameTextures[eye][gWriteSlot] != nullptr) {
        return gGameTextures[eye][gWriteSlot];
    }
    if (gCompositor.Device == nullptr || gCompositor.Width == 0 || gCompositor.Height == 0) {
        return nullptr;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatBGRA8Unorm, gCompositor.Width, gCompositor.Height, false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    gGameTextures[eye][gWriteSlot] = static_cast<MTL::Device*>(gCompositor.Device)->newTexture(descriptor);
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

void SetVisionOSRefreshRate(uint32_t hz) {
    if (hz < 30 || hz > 240 || hz == gRefreshRate) {
        return;
    }
    // The only report of the panel rate there is. Compositor Services never states it, so without
    // this line nothing says whether the app is holding 90, 96 or 120.
    SPDLOG_INFO("visionOS: presenting at {} Hz", hz);
    gRefreshRate = hz;
}

void SetVisionOSViewCount(uint32_t views) {
    gViewCount = views >= 2 ? 2 : 1;
}

GfxWindowBackendVisionOS::GfxWindowBackendVisionOS(GfxRenderingAPIMetal* renderingApi)
    : mRenderingApi(renderingApi) {
}

void GfxWindowBackendVisionOS::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                    uint32_t height, int32_t posX, int32_t posY) {
    mWidth = gCompositor.Width != 0 ? gCompositor.Width : width;
    mHeight = gCompositor.Height != 0 ? gCompositor.Height : height;
    mFullScreen = true;

    MTL::Texture* target = static_cast<MTL::Texture*>(GetVisionOSGameTexture(0));
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
    MoveGlass();
    mViewsThisFrame = gViewCount;
    return mViewsThisFrame;
}

// The eye offset converts against the glass itself, so a point on the glass lands on the same spot
// for both eyes. The gain then sets how deep the world reads: it compresses every disparity so the
// farthest thing the game draws sits the diorama depth behind the window. It stays under one,
// so the eyes cannot be made to diverge. Along the normal the plain scale stands, so the apex of
// the frustum reaches the glass exactly when the nose does.
void GfxWindowBackendVisionOS::BeginRenderView(uint32_t view) {
    gViewIndex = static_cast<int>(view);
    gViewGeometryValid = false;

    // NewFrame points framebuffer zero at whatever the external target is, once per view, so the
    // eye only has to be chosen here.
    if (mRenderingApi != nullptr) {
        mRenderingApi->MetalSetExternalTarget(static_cast<MTL::Texture*>(GetVisionOSGameTexture(gViewIndex)));
    }

    const VisionOSWindow window = GetVisionOSWindow();
    if (view >= gViewCount || window.HalfWidth <= 0.0f || window.Range <= 0.0f || gTanHalfWidth <= 0.0f) {
        return;
    }

    // One image for both eyes is drawn from between them, not from either one. The simulator hands
    // out a single view, so there is not always a second eye to be between.
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

    // Against where the head stood when the window was placed, not against the middle of the glass.
    // A window put high or to one side would otherwise be drawn as one looked into from there, and
    // would stay that way for as long as it hung there.
    gViewGeometry.eyeOffset[0] = (eyeX - gParallaxAcross) * acrossGlass;
    gViewGeometry.eyeOffset[1] = (eyeY - gParallaxRise) * acrossGlass;
    // The shell reports z towards the viewer, and the model wants how far the head has come off
    // the range the window hangs at.
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
    // The main loop calls this every pass, including the passes it spends off screen, so this is
    // where the shell can report a pause, a resume or an invalidated layer.
    if (gFrameHooks.PollState != nullptr) {
        gFrameHooks.PollState();
    }

    // The Game Controller framework reports a key on a queue of its own, so take the whole batch
    // here, on the thread that owns ImGui.
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

    // SDL has no video here, but the control deck still reads controller add and remove from
    // the same queue, and only a pump puts them there.
    SDL_PumpEvents();

    // The control deck polls the pad rather than taking its events, so nothing else empties the
    // queue. SDL_PeepEvents walks the whole of it to find the two the device handler wants, under
    // the event lock, twice a frame, so a queue that only grows makes every frame slower than the
    // last. The window backend on the desktop drops the same events for the same reason.
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_CONTROLLERDEVICEADDED - 1) > 0) {
    }
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMOVED + 1, SDL_LASTEVENT) > 0) {
    }
}

bool GfxWindowBackendVisionOS::IsFrameReady() {
    return true;
}

void GfxWindowBackendVisionOS::SwapBuffersBegin() {
    // ImGui has ended its frame by now, so the window order is settled and the mask can be put in
    // order. The shell rasterizes it inside CloseFrame.
    EndVisionOSTrackingRects();

    // A GUI-only frame reaches here without BeginRenderFrame, the same way the OpenXR backend has
    // to open one late. It asked for no views, so it is a frame of one and both eyes read it.
    if (!mFrameOpen) {
        if (!OpenFrame()) {
            return;
        }
        mViewsThisFrame = 1;
        gViewIndex = 0;
    }

    // The compositor frame holds both eyes. Close it when the last one has committed, so the shell
    // makes its command buffer after ours and the screen samples this frame, not the one before it.
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
