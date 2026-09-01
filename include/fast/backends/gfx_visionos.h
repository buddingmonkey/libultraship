#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Fast {

// The visionOS shell owns the Metal objects and says how large a picture it wants. libultraship
// draws into that picture and publishes it for the shell to show. Every pointer is an Objective-C
// object, so the shell and the backend agree on void* instead of on Metal headers.
struct VisionOSRenderTarget {
    void* Device;
    void* CommandQueue;
    uint32_t Width;
    uint32_t Height;
};

void SetVisionOSRenderTarget(void* device, void* commandQueue, uint32_t width, uint32_t height);
// One per eye, and two deep. Both eyes are drawn from the same frame, so they cannot be the same
// texture. The two slots let a shell copy the frame the game just finished while the game draws the
// next one, so no read ever meets a write. A shell that draws from the game texture itself needs
// only the first pair and never flips.
void* GetVisionOSGameTexture(int eye);
void* GetVisionOSReadyGameTexture(int eye);
void FlipVisionOSGameTextures();

// The cadence the shell is actually presenting at. Nothing on visionOS reports the panel rate and
// nothing can ask for one, so the shell measures it from the frame times and the app follows
// whatever it is given.
void SetVisionOSRefreshRate(uint32_t hz);

// How many views the shell wants this frame. One image for both eyes below two.
void SetVisionOSViewCount(uint32_t views);

// The shell owns the frame. It opens one when the backend asks, and closes it after Fast3D has
// committed, so the picture the shell shows is the frame that was just drawn.
struct VisionOSFrameHooks {
    bool (*OpenFrame)();
    void (*CloseFrame)();
    bool (*IsRunning)();
    // The scene state is the only lifecycle signal here, and it must be read even while the app
    // is off screen, when no frame is opened at all.
    void (*PollState)();
};

void SetVisionOSFrameHooks(VisionOSFrameHooks hooks);

// Where the press meets the picture, in game texture pixels.
struct VisionOSPointer {
    float X;
    float Y;
    bool Valid;
    bool Pressed;
};

// The system reports a ray only at the moment of a pinch, so a press and a new position arrive
// together. ImGui must take the position first, or the press lands on whatever was under the last
// one. Keep the samples in order and let the reader take one step per frame.
void PushVisionOSPointer(VisionOSPointer pointer);
bool PeekVisionOSPointer(VisionOSPointer* pointer);
void PopVisionOSPointer();

// The room screen, in meters. A shell that owns the size says so with SetVisionOSWindow, and the
// getter reports that. A shell that does not gets the size the menu asks for through gfx_xr_view.h.
struct VisionOSWindow {
    float HalfWidth;
    float HalfHeight;
    float Range;
};

void SetVisionOSWindow(float halfWidth, float halfHeight, float range);
VisionOSWindow GetVisionOSWindow();

// The shape of the picture the game draws, width over height. A shell that owns the window
// letterboxes to it. Zero until there is a picture to measure.
float GetVisionOSPictureAspect();

// Where the head stood when the window was placed, across and up, in the screen's own axes. The
// picture faces the viewer from there, and only what the head does afterwards is parallax. The
// range in VisionOSWindow does the same for the third axis, and sets the depth scale as well.
void SetVisionOSParallaxReference(float across, float rise);

// Where an eye is, in meters, in the room screen's own axes: x right, y up, z towards the viewer.
// The shell owns where the screen stands, so it does the world to screen part.
void SetVisionOSEye(int view, float x, float y, float z);

// A hardware keyboard reaches visionOS through the Game Controller framework. SDL keeps its
// keyboard in the video driver, and this build has no video, so SDL never sees one. The Game
// Controller key code is the HID usage, which is what an SDL scancode is.
void PushVisionOSKey(int scancode, bool pressed);

// One rectangle the system can highlight, in game texture pixels. An identifier of zero is a
// window, which takes no highlight of its own and only hides what is behind it.
struct VisionOSHoverRect {
    float MinX;
    float MinY;
    float MaxX;
    float MaxY;
    uint64_t Identifier;
};

// The set is collected while ImGui builds the frame and put in order after ImGui ends it, because
// the window order is only correct then.
void BeginVisionOSHoverRects();
void EndVisionOSHoverRects();

// The shell reads the newest set from the main thread, which is not the thread that builds it. It
// returns how many it wrote.
size_t CopyVisionOSHoverRects(VisionOSHoverRect* out, size_t max);

} // namespace Fast

#ifdef __VISIONOS__

#include <vector>
#include "gfx_window_manager_api.h"

namespace Fast {

class GfxRenderingAPIMetal;

// visionOS has no window. The shell decides the eye targets, so this backend reports one fixed
// size, owns no drawable, and leaves every window and pointer operation harmless.
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
