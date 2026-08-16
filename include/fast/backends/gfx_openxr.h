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
#include "gfx_xr_view.h"

namespace Fast {

// Presents the game on a window anchored in the room. The frame is drawn once per eye, each with
// an off-axis frustum from that eye to the window rectangle. Each eye's image is then drawn onto
// the window rectangle inside a full-view projection layer, with the room showing through the
// alpha around it. A button that opens the menu, a bar that moves the window and a handle that
// resizes it hang in that alpha, so the picture holds nothing but the game, and a pinch on the
// picture still belongs to the game. SDL keeps the window, the GLES context, audio and controllers.
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
    enum class Grab { None, Move, Resize };

    bool StartSession();
    bool StartActions(bool handInteraction);
    bool StartPadActions();
    void PumpPad();
    void ClearPointer();
    void StartPassthrough();
    void StartRefreshRates();
    void HoldRefreshRate();
    void PollEvents();
    void HandleStateChange(const XrEventDataSessionStateChanged& changed);
    void HandleReferenceSpaceChange(const XrEventDataReferenceSpaceChangePending& change);
    void PollLocalSpace();
    XrVector3f HeadPosition() const;
    bool PlaneHit(const XrPosef& pose, float* planeX, float* planeY) const;
    float DiagonalReach(float planeX, float planeY) const;
    bool OnBar(float planeX, float planeY) const;
    int OnCorner(float planeX, float planeY) const;
    void StartGrab(Grab kind, int hand, const XrVector3f& handPosition, float planeX, float planeY);
    bool UpdateGrab(XrTime displayTime);
    void EndGrab();
    void PumpPointer(XrTime displayTime);
    bool OpenFrame();
    void LocateViews();
    void SizeWindow();
    void PlaceWindow();
    void AnchorHere();
    void Recenter();
    XrVector3f ToWindowAxes(const XrVector3f& point) const;
    void MoveGlass();
    void ApplySettings();
    bool StartPlacementPass();
    float MenuSide() const;
    float MenuZone() const;
    float CursorSide() const;
    float MenuRise() const;
    float BarWidth() const;
    float BarHeight() const;
    float BarDrop() const;
    float CornerSide() const;
    float WindowCorner() const;
    float EdgeFloat() const;
    XrPosef PlanePose(float x, float y) const;
    void PresentView(uint32_t view);
    void DrawOverlays(uint32_t eye);
    void DrawEye(uint32_t eye, uint32_t sourceView);
    void EndRenderFrame();
    void Teardown();

    XrInstance mInstance = XR_NULL_HANDLE;
    XrSystemId mSystemId = XR_NULL_SYSTEM_ID;
    XrSession mSession = XR_NULL_HANDLE;
    XrSpace mSpace = XR_NULL_HANDLE;
    XrSpace mLocalSpace = XR_NULL_HANDLE;
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
    uint32_t mMenuProgram = 0;
    uint32_t mCursorProgram = 0;
    uint32_t mBarProgram = 0;
    uint32_t mCornerProgram = 0;
    uint32_t mVao = 0;
    int32_t mMvpLoc = -1;
    int32_t mAspectLoc = -1;
    int32_t mRadiusLoc = -1;
    int32_t mFeatherLoc = -1;
    int32_t mShiftLoc = -1;
    int32_t mMenuMvpLoc = -1;
    int32_t mMenuGlowLoc = -1;
    int32_t mCursorMvpLoc = -1;
    int32_t mCursorDownLoc = -1;
    int32_t mBarMvpLoc = -1;
    int32_t mBarGlowLoc = -1;
    int32_t mBarAspectLoc = -1;
    int32_t mCornerMvpLoc = -1;
    int32_t mCornerGlowLoc = -1;

    XrView mViews[VIEW_COUNT] = {};
    bool mViewsValid = false;
    XrTime mDisplayTime = 0;
    XrTime mRecenterAfter = 0;
    bool mFrameOpen = false;
    bool mShouldRender = false;
    uint32_t mViewCount = 1;
    uint32_t mCurrentView = 0;

    float mWindowWidth = 0.0f;
    float mWindowHeight = 0.0f;
    float mWindowDistance = 0.0f;
    float mWindowRadius = 0.0f;
    float mWindowScale = 1.0f;
    XrVector3f mPlacementHead = {};
    XrVector3f mWindowDir = { 0.0f, 0.0f, -1.0f };
    XrPosef mAnchorPose = {};
    XrVector3f mViewpoint = {};
    bool mAnchorValid = false;
    bool mWindowSized = false;

    Grab mGrab = Grab::None;
    int mGrabHand = 0;
    XrVector3f mGrabHandPosition = {};
    XrVector3f mGrabWindowPosition = {};
    float mGrabScale = 1.0f;
    float mGrabReach = 0.0f;
    bool mBarHover = false;
    int mCornerHover = -1;

    bool mSrgbWriteControl = false;

    PFN_xrRequestDisplayRefreshRateFB mRequestRefreshRate = nullptr;
    std::vector<float> mRefreshRates;
    float mRefreshRate = 0.0f;
    float mWantedRate = 0.0f;
    int mRateRetries = 0;

    XrPassthroughFB mPassthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB mPassthroughLayer = XR_NULL_HANDLE;
    PFN_xrDestroyPassthroughFB mDestroyPassthrough = nullptr;
    PFN_xrDestroyPassthroughLayerFB mDestroyPassthroughLayer = nullptr;

    XrActionSet mActionSet = XR_NULL_HANDLE;
    XrAction mAimAction = XR_NULL_HANDLE;
    XrAction mSelectAction = XR_NULL_HANDLE;
    XrAction mStickAction = XR_NULL_HANDLE;
    XrAction mTriggerAction = XR_NULL_HANDLE;
    XrAction mSqueezeAction = XR_NULL_HANDLE;
    XrAction mFaceLowAction = XR_NULL_HANDLE;  // A on the right hand, X on the left
    XrAction mFaceHighAction = XR_NULL_HANDLE; // B on the right hand, Y on the left
    XrAction mMenuAction = XR_NULL_HANDLE;
    XrAction mStickClickAction = XR_NULL_HANDLE;
    XrSpace mAimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    XrPath mHandPath[2] = { XR_NULL_PATH, XR_NULL_PATH };
    jobject mActivity = nullptr;
    bool mActive = false;
    bool mRunning = false;
};

} // namespace Fast

#endif
