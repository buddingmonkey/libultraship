#ifdef __VISIONOS__

#include "fast/backends/gfx_visionos.h"

#include <Metal/Metal.hpp>
#include <SDL_events.h>
#include <spdlog/spdlog.h>

#include <cmath>

#include "fast/backends/gfx_metal.h"

namespace Fast {

namespace {
VisionOSCompositor gCompositor = { nullptr, nullptr, 0, 0 };
MTL::Texture* gGameTexture = nullptr;
GfxRenderingAPIMetal* gTestRenderer = nullptr;
bool gTestRendererFailed = false;
} // namespace

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

void RenderVisionOSTestPattern(uint64_t frameIndex) {
    MTL::Texture* target = static_cast<MTL::Texture*>(GetVisionOSGameTexture());
    if (target == nullptr || gTestRendererFailed) {
        return;
    }

    if (gTestRenderer == nullptr) {
        gTestRenderer = new GfxRenderingAPIMetal();
        if (!gTestRenderer->MetalInitExternal(static_cast<MTL::Device*>(gCompositor.Device),
                                              static_cast<MTL::CommandQueue*>(gCompositor.CommandQueue), target)) {
            SPDLOG_ERROR("visionOS: the external Metal target did not come up");
            gTestRendererFailed = true;
            return;
        }
        gTestRenderer->Init();
    }

    const double phase = static_cast<double>(frameIndex % 180) / 180.0 * 2.0 * M_PI;
    gTestRenderer->SetExternalClearColor(0.5 + 0.45 * std::sin(phase), 0.10,
                                         0.5 + 0.45 * std::sin(phase + M_PI), 1.0);
    gTestRenderer->StartFrame();
    gTestRenderer->StartDrawToFramebuffer(0, 0.0f);
    gTestRenderer->ClearFramebuffer(true, true);
    gTestRenderer->EndFrame();
}

void GfxWindowBackendVisionOS::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                    uint32_t height, int32_t posX, int32_t posY) {
    mWidth = gCompositor.Width != 0 ? gCompositor.Width : width;
    mHeight = gCompositor.Height != 0 ? gCompositor.Height : height;
    mFullScreen = true;
    if (GetVisionOSGameTexture() == nullptr) {
        SPDLOG_ERROR("visionOS: the compositor was not published before the window came up");
    }
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
    return mIsRunning;
}

void GfxWindowBackendVisionOS::Destroy() {
    mIsRunning = false;
}

bool GfxWindowBackendVisionOS::IsFullscreen() {
    return true;
}
} // namespace Fast
#endif
