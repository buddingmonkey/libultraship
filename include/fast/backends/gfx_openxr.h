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
#include "gfx_xr_pointer.h"
#include "gfx_xr_view.h"

namespace Fast {

// Presents the game on a window anchored in the room. The frame is drawn once per eye, each with
// an off-axis frustum from that eye to the window rectangle. Each eye's image is then drawn onto
// the window rectangle inside a full-view projection layer, with the room showing through the
// alpha around it. SDL keeps the window, the GLES context, audio and controllers.
class GfxWindowBackendOpenXR final : public GfxWindowBackendSDL2 {
  public:
    static constexpr uint32_t VIEW_COUNT = 2;

    GfxWindowBackendOpenXR() = default;
    ~GfxWindowBackendOpenXR() override;

    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    std::vector<float> GetSupportedRefreshRates() override;
    bool SetRefreshRate(float rate) override;
    uint32_t BeginRenderFrame() override;
    void BeginRenderView(uint32_t view) override;
    void SwapBuffersBegin() override;
    void Destroy() override;

  private:
    bool StartSession();
    bool StartActions(bool handInteraction);
    void StartRefreshRates();
    void PollEvents();
    void HandleStateChange(const XrEventDataSessionStateChanged& changed);
    void PumpPointer(XrTime displayTime);
    bool OpenFrame();
    void LocateViews();
    void Recentre();
    XrVector3f ToWindowAxes(const XrVector3f& point) const;
    void MoveGlass();
    void SizeWindow();
    bool StartPlacementPass();
    void PresentView(uint32_t view);
    void DrawEye(uint32_t eye, uint32_t sourceView);
    void EndRenderFrame();
    void Teardown();

    XrInstance mInstance = XR_NULL_HANDLE;
    XrSystemId mSystemId = XR_NULL_SYSTEM_ID;
    XrSession mSession = XR_NULL_HANDLE;
    XrSpace mSpace = XR_NULL_HANDLE;
    XrReferenceSpaceType mSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrSpace mAnchorSpace = XR_NULL_HANDLE;
    PFN_xrCreateAnchorSpaceANDROID mCreateAnchorSpace = nullptr;
    XrSwapchain mSwapchain[VIEW_COUNT] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    XrSessionState mState = XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode mBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    std::vector<XrSwapchainImageOpenGLESKHR> mImages[VIEW_COUNT];
    std::vector<uint32_t> mImageFbos[VIEW_COUNT];
    uint32_t mSwapchainWidth = 0;
    uint32_t mSwapchainHeight = 0;

    uint32_t mGameWidth = 0;
    uint32_t mGameHeight = 0;
    uint32_t mGameTex[VIEW_COUNT] = { 0, 0 };
    uint32_t mGameFbo[VIEW_COUNT] = { 0, 0 };
    uint32_t mProgram = 0;
    uint32_t mVao = 0;
    int32_t mMvpLoc = -1;

    XrView mViews[VIEW_COUNT] = {};
    bool mViewsValid = false;
    XrTime mDisplayTime = 0;
    bool mFrameOpen = false;
    bool mShouldRender = false;
    uint32_t mViewCount = 1;
    uint32_t mCurrentView = 0;

    float mWindowWidth = 0.0f;
    float mWindowHeight = 0.0f;
    float mWindowDistance = 0.0f;
    XrPosef mAnchorPose = {};
    XrVector3f mViewpoint = {};
    float mAnchorYaw = 0.0f;
    bool mAnchorValid = false;
    bool mWindowSized = false;

    bool mSrgbWriteControl = false;

    PFN_xrRequestDisplayRefreshRateFB mRequestRefreshRate = nullptr;
    std::vector<float> mRefreshRates;
    float mRefreshRate = 0.0f;

    XrActionSet mActionSet = XR_NULL_HANDLE;
    XrAction mAimAction = XR_NULL_HANDLE;
    XrAction mSelectAction = XR_NULL_HANDLE;
    XrSpace mAimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    XrPath mHandPath[2] = { XR_NULL_PATH, XR_NULL_PATH };
    jobject mActivity = nullptr;
    bool mActive = false;
    bool mRunning = false;
};

} // namespace Fast

#endif
