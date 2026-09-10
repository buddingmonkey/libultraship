#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Fast {

struct VisionOSRenderTarget {
    void* Device;
    void* CommandQueue;
    uint32_t Width;
    uint32_t Height;
};

void SetVisionOSRenderTarget(void* device, void* commandQueue, uint32_t width, uint32_t height);
void* GetVisionOSGameTexture(int eye);
void* GetVisionOSReadyGameTexture(int eye);
void FlipVisionOSGameTextures();

void ReportVisionOS(const char* text);

void SetVisionOSRefreshRate(uint32_t hz);

void SetVisionOSViewCount(uint32_t views);

struct VisionOSFrameHooks {
    bool (*OpenFrame)();
    void (*CloseFrame)();
    bool (*IsRunning)();
    void (*PollState)();
};

void SetVisionOSFrameHooks(VisionOSFrameHooks hooks);

struct VisionOSPointer {
    float X;
    float Y;
    bool Valid;
    bool Pressed;
};

void PushVisionOSPointer(VisionOSPointer pointer);
bool PeekVisionOSPointer(VisionOSPointer* pointer);
void PopVisionOSPointer();

struct VisionOSWindow {
    float HalfWidth;
    float HalfHeight;
    float Range;
};

void SetVisionOSWindow(float halfWidth, float halfHeight, float range);
VisionOSWindow GetVisionOSWindow();

float GetVisionOSPictureAspect();

void SetVisionOSParallaxReference(float across, float rise);

void SetVisionOSEye(int view, float x, float y, float z);

void PushVisionOSKey(int scancode, bool pressed);

struct VisionOSHoverRect {
    float MinX;
    float MinY;
    float MaxX;
    float MaxY;
    uint64_t Identifier;
};

void BeginVisionOSHoverRects();
void EndVisionOSHoverRects();

size_t CopyVisionOSHoverRects(VisionOSHoverRect* out, size_t max);

} // namespace Fast

#ifdef __VISIONOS__

#include <vector>
#include "gfx_window_manager_api.h"

namespace Fast {

class GfxRenderingAPIMetal;

class GfxWindowBackendVisionOS final : public GfxWindowBackend {
  public:
    explicit GfxWindowBackendVisionOS(GfxRenderingAPIMetal* renderingApi);
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
    uint32_t BeginRenderFrame() override;
    void BeginRenderView(uint32_t view) override;
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
    bool OpenFrame();

    GfxRenderingAPIMetal* mRenderingApi = nullptr;
    void (*mOnAllKeysUp)() = nullptr;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mViewsThisFrame = 1;
    bool mFrameOpen = false;
};

} // namespace Fast

#endif
