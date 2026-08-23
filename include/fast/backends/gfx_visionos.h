#pragma once

#include <stdint.h>

namespace Fast {

// The visionOS shell owns the Compositor Services layer and its Metal objects. libultraship
// borrows them and publishes the texture that the room screen samples. Every pointer is an
// Objective-C object, so the shell and the backend agree on void* instead of on Metal headers.
struct VisionOSCompositor {
    void* Device;
    void* CommandQueue;
    uint32_t Width;
    uint32_t Height;
};

void SetVisionOSCompositor(void* device, void* commandQueue, uint32_t width, uint32_t height);
VisionOSCompositor GetVisionOSCompositor();
void* GetVisionOSGameTexture();

// Proves that Fast3D owns the game texture before the game itself starts. Step 4 removes it.
void RenderVisionOSTestPattern(uint64_t frameIndex);

} // namespace Fast

#ifdef __VISIONOS__

#include <vector>
#include "gfx_window_manager_api.h"

namespace Fast {

// visionOS has no window. The compositor decides the eye targets, so this backend reports one
// fixed size, owns no drawable, and leaves every window and pointer operation harmless.
class GfxWindowBackendVisionOS final : public GfxWindowBackend {
  public:
    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void Close() override;
    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override;
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool isNowFullscreen)) override;
    void SetFullscreen(bool fullscreen) override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    void SetCursorVisibility(bool visible) override;
    void SetMousePos(int32_t posX, int32_t posY) override;
    void GetMousePos(int32_t* x, int32_t* y) override;
    void GetMouseDelta(int32_t* x, int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override;
    void SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) override;
    Ship::WindowRect GetPrimaryMonitorRect() override;
    void HandleEvents() override;
    bool IsFrameReady() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;
    double GetTime() override;
    int GetTargetFps() override;
    void SetTargetFps(int fps) override;
    void SetMaxFrameLatency(int latency) override;
    const char* GetKeyName(int scancode) override;
    bool CanDisableVsync() override;
    bool IsRunning() override;
    void Destroy() override;
    bool IsFullscreen() override;

  private:
    void (*mOnAllKeysUp)() = nullptr;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
};

} // namespace Fast

#endif
