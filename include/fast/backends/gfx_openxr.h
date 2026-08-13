#pragma once

#ifdef ENABLE_OPENXR

#include <cstdint>
#include <vector>

#include <jni.h>
#include <EGL/egl.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// gfx_sdl.h names SDL types without including SDL itself.
#include <SDL2/SDL.h>
#include "gfx_sdl.h"

namespace Fast {

// Presents the frame the game already drew on a quad anchored in the room. SDL keeps the window,
// the GLES context, audio and controllers; only the presentation changes.
class GfxWindowBackendOpenXR final : public GfxWindowBackendSDL2 {
  public:
    GfxWindowBackendOpenXR() = default;
    ~GfxWindowBackendOpenXR() override;

    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void SwapBuffersBegin() override;
    void Destroy() override;

  private:
    bool StartSession();
    void PollEvents();
    void HandleStateChange(const XrEventDataSessionStateChanged& changed);
    void PresentQuad();
    void Teardown();

    XrInstance mInstance = XR_NULL_HANDLE;
    XrSystemId mSystemId = XR_NULL_SYSTEM_ID;
    XrSession mSession = XR_NULL_HANDLE;
    XrSpace mSpace = XR_NULL_HANDLE;
    XrSwapchain mSwapchain = XR_NULL_HANDLE;
    XrSessionState mState = XR_SESSION_STATE_UNKNOWN;

    std::vector<XrSwapchainImageOpenGLESKHR> mImages;
    std::vector<uint32_t> mImageFbos;
    uint32_t mSwapchainWidth = 0;
    uint32_t mSwapchainHeight = 0;

    jobject mActivity = nullptr;
    bool mActive = false;
    bool mRunning = false;
};

} // namespace Fast

#endif
