#pragma once

#include <stddef.h>
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

// The shell owns the Compositor Services frame. It opens one when the backend asks, and closes it
// after Fast3D has committed, so the screen samples the frame that was just drawn.
struct VisionOSFrameHooks {
    bool (*OpenFrame)();
    void (*CloseFrame)();
    bool (*IsRunning)();
};

void SetVisionOSFrameHooks(VisionOSFrameHooks hooks);

// Where the gaze and pinch ray meets the room screen, in game texture pixels. The system gives a
// ray only while a pinch is held, so there is no position to report before the press.
struct VisionOSPointer {
    float X;
    float Y;
    uint64_t Identifier; ///< The tracking area the system aimed at, which is the ImGui item id.
    bool Valid;
    bool Pressed;
};

// The system reports a ray only at the moment of a pinch, so a press and a new position arrive
// together. ImGui must take the position first, or the press lands on whatever was under the last
// one. Keep the samples in order and let the reader take one step per frame.
void PushVisionOSPointer(VisionOSPointer pointer);
bool PeekVisionOSPointer(VisionOSPointer* pointer);
void PopVisionOSPointer();

void SetVisionOSItemLabel(uint64_t identifier, const char* label);
const char* GetVisionOSItemLabel(uint64_t identifier);

// One rectangle of the tracking mask, in game texture pixels. An identifier of zero is a window,
// which takes no tracking area and only hides what is behind it.
struct VisionOSTrackingRect {
    float MinX;
    float MinY;
    float MaxX;
    float MaxY;
    uint64_t Identifier;
};

// The mask is collected while ImGui builds the frame and put in order after ImGui ends it, because
// the window order is only correct then.
void BeginVisionOSTrackingRects();
void EndVisionOSTrackingRects();
size_t GetVisionOSTrackingRectCount();
VisionOSTrackingRect GetVisionOSTrackingRect(size_t index);

} // namespace Fast

#ifdef __VISIONOS__

#include <vector>
#include "gfx_window_manager_api.h"

namespace Fast {

class GfxRenderingAPIMetal;

// visionOS has no window. The compositor decides the eye targets, so this backend reports one
// fixed size, owns no drawable, and leaves every window and pointer operation harmless.
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
    bool mFrameOpen = false;
};

} // namespace Fast

#endif
