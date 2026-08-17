#ifdef ENABLE_OPENXR

#include "fast/backends/gfx_openxr.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include <android/log.h>
#include <GLES3/gl3.h>

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#ifdef ENABLE_DEBUG_TOOLS
#include "fast/backends/gfx_debug_capture.h"
#include "fast/backends/gfx_debug_pointer.h"
#endif
#include "ship/Context.h"
#include "ship/window/MouseStateManager.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include "ship/window/gui/GuiWindow.h"

namespace Fast {

// The window hangs in front of where the user faced at start, at the range the setting asks for.
// The default is the range the common headsets focus at, so the glass, the frame and the HUD ask
// the eyes for the vergence the optics already ask for.
static constexpr float WINDOW_DISTANCE_DEFAULT = 1.3f;
static constexpr float WINDOW_DISTANCE_MIN = 0.5f;
static constexpr float WINDOW_DISTANCE_MAX = 4.0f;
// The half-angle the window covers across, before the game has loaded a projection to ask for its
// own. About 61 degrees, which is what Banjo-Kazooie asks for once it runs.
static constexpr float WINDOW_TAN_HALF_WIDTH_DEFAULT = 0.59f;

// How much of the window's own width the game draws, in steps, and what it draws over the width the
// window covers in the view.
static constexpr float RENDER_STEPS = 16.0f;
static constexpr float RENDER_HEADROOM = 1.2f;

// The size is meters of glass, and the range does not touch it: a window pushed away keeps the
// width it had and goes small in the eye, as everything else in the room does. Scale 1 is the width
// the game's field of view gives at the range below, and the largest scale fills the same angle
// from the furthest range.
static constexpr float WINDOW_SIZE_RANGE = 0.5f;
static constexpr float WINDOW_SCALE_MIN = 0.5f;
static constexpr float WINDOW_SCALE_MAX = 8.0f;
// The scale that covers the same angle at the default range as scale 1 does at the nearest one.
static constexpr float WINDOW_SCALE_DEFAULT = WINDOW_DISTANCE_DEFAULT / WINDOW_SIZE_RANGE;

// Meters of world behind the glass: the farthest thing the game draws sits this far behind the
// window, and everything nearer sorts itself in between. The gain it makes, depth over range plus
// depth, stays under one, so no range and no size can put more than one eye separation of parallax
// on the glass and the eyes never diverge.
static constexpr float DIORAMA_DEPTH_DEFAULT = 2.0f;
static constexpr float DIORAMA_DEPTH_MIN = 0.5f;
static constexpr float DIORAMA_DEPTH_MAX = 4.0f;

// How far above or below the eyes the window can go, in radians, and where it starts to bend in to
// face the user. Below the first figure it stands upright. The second is the end of the capsule,
// and it is short of a quarter turn on purpose: a window that faces the user has no yaw at all
// straight overhead.
static constexpr float WINDOW_RISE_FLAT = 0.35f;
static constexpr float WINDOW_RISE_MAX = 1.22f;

// Game units from the viewpoint to the glass, which is also the scale: this many game units fill
// one window distance of the room. A flat quad cannot hold anything nearer than the glass, so the
// glass goes at the nearest thing the game drew and never deeper than the range Banjo stands at.
static constexpr float WINDOW_DEPTH_MAX = 700.0f;
static constexpr float WINDOW_DEPTH_MIN = 1.0f;

// Seconds for the glass to come back out. Long, so it holds about the nearest of the last few
// seconds instead of chasing the camera in and out of every corner.
static constexpr float WINDOW_DEPTH_RELEASE = 2.0f;

// The glass keeps this much clear of the nearest thing. The reading is a frame old, so without the
// margin anything moving towards the camera crosses the glass for a frame before it moves.
static constexpr float WINDOW_DEPTH_MARGIN = 0.9f;

// The menu button, in window heights: how large it is drawn, how far above the top edge it sits,
// and how much larger than the drawing the rectangle that takes the pinch is. A target in the air
// needs more than its outline, and the gap is wider than the growth, so the button and the picture
// never contest the same pinch.
static constexpr float MENU_BUTTON_SIDE = 0.06f;
static constexpr float MENU_BUTTON_GAP = 0.04f;
static constexpr float MENU_BUTTON_REACH = 1.6f;

// The cursor shows in a zone this many button sides wide around the button. The zone reaches down
// past the top edge of the window, so the cursor never goes out while the hand crosses from the
// picture to the button, which is the only place a small target is hard to find.
static constexpr float MENU_BUTTON_ZONE = 3.0f;

// The cursor, in window heights. The rest of the rectangle holds the shadow. The ring sits at this
// much of the rectangle, which is what the ray keeps its distance from; CURSOR_FRAGMENT draws it.
static constexpr float CURSOR_SIDE = 0.075f;
static constexpr float CURSOR_RING = 0.32f;

// The ray, in meters. A dot on its own says nothing about which hand put it there, and both hands
// can carry one. It is a stub out of the hand rather than a line to the dot: hidden for the first
// stretch, so it does not sit on the front of a controller, and stopped short of the dot, so it
// never runs into the middle of it. Where the dot is nearer than the reach, the gap wins.
static constexpr float RAY_HIDDEN = 0.06f;
static constexpr float RAY_RAMP = 0.01f;
static constexpr float RAY_REACH = 0.50f;
static constexpr float RAY_GAP = 0.02f;
// Held at the width it has at the hand, so perspective does not narrow it. What narrows it is the
// taper, which begins this far before the far fade and takes it to this much of its width by the
// end. Raise the first number to start the taper nearer the hand.
static constexpr float RAY_WIDTH = 0.00125f;
static constexpr float RAY_TAPER = 0.02f;
static constexpr float RAY_TAPER_TO = 0.4f;

// What the ray and the dot turn while the trigger is held. A color rather than more opacity: the
// pointer hangs over passthrough as readily as over the game, and a white that is only brighter
// says nothing against a bright picture. One definition, because two shaders read it.
static constexpr float POINTER_IDLE[3] = { 1.0f, 1.0f, 1.0f };
static constexpr float POINTER_HELD[3] = { 0.20f, 0.55f, 1.0f };

// The move bar, in window widths and window heights: how wide and how tall it is drawn, how far
// below the bottom edge it hangs, and how much taller than the drawing the rectangle that takes the
// pinch is. A bar that thin needs the extra height to be reachable, and the gap is wider than the
// growth, so the bar and the picture never contest the same pinch.
static constexpr float MOVE_BAR_WIDTH = 0.22f;
static constexpr float MOVE_BAR_HEIGHT = 0.022f;
static constexpr float MOVE_BAR_GAP = 0.035f;
static constexpr float MOVE_BAR_REACH = 2.6f;

// The corner handle, in window heights: how large the drawing is, and how far outside the corner
// the pinch reaches. The target is the part of that square outside the picture, so a pinch near a
// corner of the game view still belongs to the game.
static constexpr float CORNER_SIDE = 0.16f;
static constexpr float CORNER_ZONE = 0.11f;

// The handle's arc turns about a center this far inside the corner, in corner sides: the RADIUS
// less the GAP of the shader below. The picture rounds onto the same center, so the arc the handle
// draws follows the corner it takes hold of, a constant gap outside it.
static constexpr float CORNER_ARC_CENTER = 0.155f;

// How far in from its edge the picture fades out, in window heights, before the softness setting,
// and the most that setting can multiply it by.
static constexpr float WINDOW_FEATHER = 0.012f;
static constexpr float EDGE_SOFTNESS_MAX = 3.0f;

// Each eye looks through the window at its own angle, so at a side edge one eye sees a sliver of
// the world the other cannot. That is what a real window does, and no mask removes it. What makes
// it hard to look at is that the edge stands at the depth of the nearest thing behind it, so it is
// not clearly in front of what it cuts. The cure is the floating window of stereo film: each eye
// gives up its own side edge, which puts crossed parallax on it and floats the frame towards the
// viewer. The sliver is then plainly behind a near edge, which the eyes read as ordinary occlusion.
//
// The crop is this fraction of the eye separation, so it holds its depth whoever wears the headset.
// One puts the frame at half the range of the glass, which is far more than film ever uses.
static constexpr float EDGE_FLOAT_MAX = 1.0f;

// Radians of turn in a new LOCAL origin below which it reads as the system keeping the space near
// the user rather than the user asking for a recenter. A gesture made while facing the old front
// falls under it, and costs nothing: the window is already there.
static constexpr float RECENTER_YAW_MIN = 0.035f;

static float sWindowDistance = WINDOW_DISTANCE_DEFAULT;
static float sWindowScale = WINDOW_SCALE_DEFAULT;
static float sDioramaDepth = DIORAMA_DEPTH_DEFAULT;

static float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

static float Smoothstep(float low, float high, float value) {
    const float t = Clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static bool Failed(XrInstance instance, XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return false;
    }
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, name))) {
        snprintf(name, sizeof(name), "XrResult(%d)", (int)result);
    }
    SPDLOG_ERROR("OpenXR: {} failed with {}", what, name);
    return true;
}

static float YawOf(const XrQuaternionf& q) {
    return atan2f(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

static bool HasExtension(const std::vector<XrExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions) {
        if (strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

GfxWindowBackendOpenXR::~GfxWindowBackendOpenXR() {
    Teardown();
}

void GfxWindowBackendOpenXR::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                  uint32_t height, int32_t posX, int32_t posY) {
    GfxWindowBackendSDL2::Init(gameName, apiName, startFullScreen, width, height, posX, posY);

    mActive = StartSession();
    if (!mActive) {
        SPDLOG_ERROR("OpenXR: no session; the game stays on the flat panel");
        Teardown();
    }
}

bool GfxWindowBackendOpenXR::StartSession() {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (XR_FAILED(
            xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initializeLoader)) ||
        initializeLoader == nullptr) {
        SPDLOG_ERROR("OpenXR: the loader has no xrInitializeLoaderKHR");
        return false;
    }

    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    JavaVM* vm = nullptr;
    if (env == nullptr || env->GetJavaVM(&vm) != 0) {
        SPDLOG_ERROR("OpenXR: SDL gave no JNI environment");
        return false;
    }

    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (activity == nullptr) {
        SPDLOG_ERROR("OpenXR: SDL gave no activity");
        return false;
    }
    mActivity = env->NewGlobalRef(activity);
    env->DeleteLocalRef(activity);

    XrLoaderInitInfoAndroidKHR loaderInit{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    loaderInit.applicationVM = vm;
    loaderInit.applicationContext = mActivity;
    if (Failed(XR_NULL_HANDLE, initializeLoader((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInit),
               "xrInitializeLoaderKHR")) {
        return false;
    }

    uint32_t count = 0;
    if (Failed(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
               "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    std::vector<XrExtensionProperties> extensions(count, { XR_TYPE_EXTENSION_PROPERTIES });
    if (Failed(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()),
               "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    if (!HasExtension(extensions, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME) ||
        !HasExtension(extensions, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) {
        SPDLOG_ERROR("OpenXR: the runtime has no GLES binding");
        return false;
    }

    const bool handInteraction = HasExtension(extensions, "XR_EXT_hand_interaction");
    const bool refreshRate = HasExtension(extensions, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

    std::vector<const char*> enabled = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                         XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    if (handInteraction) {
        enabled.push_back("XR_EXT_hand_interaction");
    }
    if (refreshRate) {
        enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    const bool unbounded = HasExtension(extensions, "XR_ANDROID_unbounded_reference_space");
    if (unbounded) {
        enabled.push_back("XR_ANDROID_unbounded_reference_space");
    }
    const bool trackables = HasExtension(extensions, XR_ANDROID_TRACKABLES_EXTENSION_NAME);
    if (trackables) {
        enabled.push_back(XR_ANDROID_TRACKABLES_EXTENSION_NAME);
    }
    // Horizon OS offers no ALPHA_BLEND environment on a stereo view, so the room can only come
    // from a passthrough layer under the frame.
    const bool passthrough = HasExtension(extensions, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (passthrough) {
        enabled.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    }

    XrInstanceCreateInfoAndroidKHR androidInfo{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    androidInfo.applicationVM = vm;
    androidInfo.applicationActivity = mActivity;

    XrInstanceCreateInfo instanceInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.next = &androidInfo;
    snprintf(instanceInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "Lighthouse");
    instanceInfo.applicationInfo.applicationVersion = 1;
    snprintf(instanceInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "Fast3D");
    instanceInfo.applicationInfo.engineVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instanceInfo.enabledExtensionCount = (uint32_t)enabled.size();
    instanceInfo.enabledExtensionNames = enabled.data();
    if (Failed(XR_NULL_HANDLE, xrCreateInstance(&instanceInfo, &mInstance), "xrCreateInstance")) {
        return false;
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (Failed(mInstance, xrGetSystem(mInstance, &systemInfo, &mSystemId), "xrGetSystem")) {
        return false;
    }

    // The spec requires this call before xrCreateSession, even though the result goes unused.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    if (Failed(mInstance,
               xrGetInstanceProcAddr(mInstance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                     (PFN_xrVoidFunction*)&getRequirements),
               "xrGetInstanceProcAddr(graphics requirements)")) {
        return false;
    }
    XrGraphicsRequirementsOpenGLESKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    if (Failed(mInstance, getRequirements(mInstance, mSystemId, &requirements),
               "xrGetOpenGLESGraphicsRequirementsKHR")) {
        return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    binding.display = eglGetCurrentDisplay();
    binding.context = eglGetCurrentContext();
    if (binding.display == EGL_NO_DISPLAY || binding.context == EGL_NO_CONTEXT) {
        SPDLOG_ERROR("OpenXR: SDL has no current EGL context on this thread");
        return false;
    }
    EGLint configId = 0;
    eglQueryContext(binding.display, binding.context, EGL_CONFIG_ID, &configId);
    const EGLint configAttributes[] = { EGL_CONFIG_ID, configId, EGL_NONE };
    EGLint configCount = 0;
    if (!eglChooseConfig(binding.display, configAttributes, &binding.config, 1, &configCount) || configCount == 0) {
        SPDLOG_ERROR("OpenXR: could not recover the EGLConfig SDL used");
        return false;
    }

    XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &binding;
    sessionInfo.systemId = mSystemId;
    if (Failed(mInstance, xrCreateSession(mInstance, &sessionInfo, &mSession), "xrCreateSession")) {
        return false;
    }

    // LOCAL on Android XR follows the head position, so nothing put in it stands still in the
    // room. UNBOUNDED is the space that does.
    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType =
        unbounded ? XR_REFERENCE_SPACE_TYPE_UNBOUNDED_ANDROID : XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (unbounded && XR_FAILED(xrCreateReferenceSpace(mSession, &spaceInfo, &mSpace))) {
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    }
    if (mSpace == XR_NULL_HANDLE &&
        Failed(mInstance, xrCreateReferenceSpace(mSession, &spaceInfo, &mSpace), "xrCreateReferenceSpace")) {
        return false;
    }
    mSpaceType = spaceInfo.referenceSpaceType;

    // The recenter gesture re-origins LOCAL, which the window does not hang in. PollLocalSpace
    // locates this one every frame, and that is what makes the runtime announce the gesture.
    if (mSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL) {
        XrReferenceSpaceCreateInfo localInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        localInfo.poseInReferenceSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateReferenceSpace(mSession, &localInfo, &mLocalSpace))) {
            mLocalSpace = XR_NULL_HANDLE;
        }
    }

    if (trackables && XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrCreateAnchorSpaceANDROID",
                                                      (PFN_xrVoidFunction*)&mCreateAnchorSpace))) {
        mCreateAnchorSpace = nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "reference space %s",
                        spaceInfo.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL" : "UNBOUNDED");

    // Alpha blend puts the room behind the quad. Opaque fills everything the quad does not cover
    // with black, which reads as a screen in a void.
    uint32_t blendModeCount = 0;
    if (!Failed(mInstance,
                xrEnumerateEnvironmentBlendModes(mInstance, mSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                                 &blendModeCount, nullptr),
                "xrEnumerateEnvironmentBlendModes")) {
        std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);
        if (!Failed(mInstance,
                    xrEnumerateEnvironmentBlendModes(mInstance, mSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                     blendModeCount, &blendModeCount, blendModes.data()),
                    "xrEnumerateEnvironmentBlendModes")) {
            mBlendMode = blendModes.empty() ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : blendModes[0];
            for (XrEnvironmentBlendMode mode : blendModes) {
                if (mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
                    mBlendMode = mode;
                    break;
                }
            }
        }
    }

    if (passthrough && mBlendMode != XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
        StartPassthrough();
    }

    uint32_t formatCount = 0;
    if (Failed(mInstance, xrEnumerateSwapchainFormats(mSession, 0, &formatCount, nullptr),
               "xrEnumerateSwapchainFormats")) {
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (Failed(mInstance, xrEnumerateSwapchainFormats(mSession, formatCount, &formatCount, formats.data()),
               "xrEnumerateSwapchainFormats")) {
        return false;
    }
    // The game draws display-referred color. A linear swapchain makes the runtime encode it to
    // sRGB on the way to the display; an sRGB one makes the blit encode it. Either washes the
    // picture out. The fix is an sRGB swapchain, which the runtime reads correctly, with the write
    // conversion turned off so the blit copies the bytes as they are.
    const char* glExtensions = (const char*)glGetString(GL_EXTENSIONS);
    mSrgbWriteControl = glExtensions != nullptr && strstr(glExtensions, "GL_EXT_sRGB_write_control") != nullptr;

    const int64_t wanted = mSrgbWriteControl ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    int64_t format = formats.empty() ? 0 : formats[0];
    for (int64_t candidate : formats) {
        if (candidate == wanted) {
            format = candidate;
            break;
        }
    }

    uint32_t width = 0;
    uint32_t height = 0;
    int32_t ignoredX = 0;
    int32_t ignoredY = 0;
    GetDimensions(&width, &height, &ignoredX, &ignoredY);
    mGameWidth = width;
    mGameHeight = height;

    XrViewConfigurationView configViews[VIEW_COUNT] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW },
                                                        { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t viewConfigCount = 0;
    if (Failed(mInstance,
               xrEnumerateViewConfigurationViews(mInstance, mSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 VIEW_COUNT, &viewConfigCount, configViews),
               "xrEnumerateViewConfigurationViews")) {
        return false;
    }
    mSwapchainWidth = configViews[0].recommendedImageRectWidth;
    mSwapchainHeight = configViews[0].recommendedImageRectHeight;

    uint32_t imageCount = 0;
    for (uint32_t view = 0; view < VIEW_COUNT; view++) {
        XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.format = format;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.width = mSwapchainWidth;
        swapchainInfo.height = mSwapchainHeight;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;
        if (Failed(mInstance, xrCreateSwapchain(mSession, &swapchainInfo, &mSwapchain[view]), "xrCreateSwapchain")) {
            return false;
        }

        if (Failed(mInstance, xrEnumerateSwapchainImages(mSwapchain[view], 0, &imageCount, nullptr),
                   "xrEnumerateSwapchainImages")) {
            return false;
        }
        mImages[view].assign(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
        if (Failed(mInstance,
                   xrEnumerateSwapchainImages(mSwapchain[view], imageCount, &imageCount,
                                              (XrSwapchainImageBaseHeader*)mImages[view].data()),
                   "xrEnumerateSwapchainImages")) {
            return false;
        }

        mImageFbos[view].resize(imageCount);
        glGenFramebuffers(imageCount, mImageFbos[view].data());
        for (uint32_t i = 0; i < imageCount; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, mImageFbos[view][i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mImages[view][i].image, 0);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // The frame the game gives back until the first head pose says how much of the view the window
    // covers. SizeRender shrinks it as soon as it can.
    CreateGameTargets(mGameWidth, mGameHeight);

    if (!StartPlacementPass()) {
        return false;
    }

    mWindowRadius = sWindowDistance;
    mWindowScale = sWindowScale;
    SizeWindow();

    for (XrView& view : mViews) {
        view = { XR_TYPE_VIEW };
    }

    if (refreshRate) {
        StartRefreshRates();
    }

    if (!StartActions(handInteraction)) {
        __android_log_print(ANDROID_LOG_ERROR, "LighthouseXR", "pointer actions failed; pinch will not reach the game");
    }

    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "session up: %u images %ux%u per eye, format 0x%04x, blend %d, srgb ctl %d", imageCount,
                        mSwapchainWidth, mSwapchainHeight, (unsigned)format, (int)mBlendMode, (int)mSrgbWriteControl);
    return true;
}

void GfxWindowBackendOpenXR::CreateGameTargets(uint32_t width, uint32_t height) {
    mTexWidth = width < 1 ? 1 : width;
    mTexHeight = height < 1 ? 1 : height;

    for (uint32_t view = 0; view < VIEW_COUNT; view++) {
        if (mGameFbo[view] != 0) {
            glDeleteFramebuffers(1, &mGameFbo[view]);
        }
        if (mGameTex[view] != 0) {
            glDeleteTextures(1, &mGameTex[view]);
        }
        glGenTextures(1, &mGameTex[view]);
        glBindTexture(GL_TEXTURE_2D, mGameTex[view]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mTexWidth, mTexHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &mGameFbo[view]);
        glBindFramebuffer(GL_FRAMEBUFFER, mGameFbo[view]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mGameTex[view], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GfxWindowBackendOpenXR::StartPassthrough() {
    PFN_xrCreatePassthroughFB create = nullptr;
    PFN_xrCreatePassthroughLayerFB createLayer = nullptr;
    PFN_xrPassthroughStartFB start = nullptr;
    PFN_xrPassthroughLayerResumeFB resume = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrCreatePassthroughFB", (PFN_xrVoidFunction*)&create)) ||
        XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction*)&createLayer)) ||
        XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrPassthroughStartFB", (PFN_xrVoidFunction*)&start)) ||
        XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrPassthroughLayerResumeFB", (PFN_xrVoidFunction*)&resume)) ||
        XR_FAILED(
            xrGetInstanceProcAddr(mInstance, "xrDestroyPassthroughFB", (PFN_xrVoidFunction*)&mDestroyPassthrough)) ||
        XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrDestroyPassthroughLayerFB",
                                        (PFN_xrVoidFunction*)&mDestroyPassthroughLayer))) {
        mDestroyPassthrough = nullptr;
        mDestroyPassthroughLayer = nullptr;
        return;
    }

    XrPassthroughCreateInfoFB info{ XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
    info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    if (Failed(mInstance, create(mSession, &info, &mPassthrough), "xrCreatePassthroughFB")) {
        mPassthrough = XR_NULL_HANDLE;
        return;
    }

    XrPassthroughLayerCreateInfoFB layerInfo{ XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
    layerInfo.passthrough = mPassthrough;
    layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    if (Failed(mInstance, createLayer(mSession, &layerInfo, &mPassthroughLayer), "xrCreatePassthroughLayerFB")) {
        mPassthroughLayer = XR_NULL_HANDLE;
        mDestroyPassthrough(mPassthrough);
        mPassthrough = XR_NULL_HANDLE;
        return;
    }

    start(mPassthrough);
    resume(mPassthroughLayer);
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "the room comes from a passthrough layer");
}

void GfxWindowBackendOpenXR::StartRefreshRates() {
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
    PFN_xrGetDisplayRefreshRateFB get = nullptr;
    if (XR_FAILED(
            xrGetInstanceProcAddr(mInstance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&enumerate)) ||
        XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&get)) ||
        XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrRequestDisplayRefreshRateFB",
                                        (PFN_xrVoidFunction*)&mRequestRefreshRate))) {
        mRequestRefreshRate = nullptr;
        return;
    }

    uint32_t count = 0;
    if (Failed(mInstance, enumerate(mSession, 0, &count, nullptr), "xrEnumerateDisplayRefreshRatesFB")) {
        return;
    }
    mRefreshRates.resize(count);
    if (Failed(mInstance, enumerate(mSession, count, &count, mRefreshRates.data()),
               "xrEnumerateDisplayRefreshRatesFB")) {
        mRefreshRates.clear();
        return;
    }
    Failed(mInstance, get(mSession, &mRefreshRate), "xrGetDisplayRefreshRateFB");

    char list[128] = "";
    for (float rate : mRefreshRates) {
        char one[16];
        snprintf(one, sizeof(one), "%s%.0f", list[0] == '\0' ? "" : " ", rate);
        strncat(list, one, sizeof(list) - strlen(list) - 1);
    }
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "display runs at %.0f Hz, offers %s", mRefreshRate, list);
}

std::vector<float> GfxWindowBackendOpenXR::GetSupportedRefreshRates() {
    return mRefreshRates;
}

bool GfxWindowBackendOpenXR::SetRefreshRate(float rate) {
    if (mRequestRefreshRate == nullptr || mSession == XR_NULL_HANDLE) {
        return false;
    }
    if (Failed(mInstance, mRequestRefreshRate(mSession, rate), "xrRequestDisplayRefreshRateFB")) {
        return false;
    }
    mRefreshRate = rate;
    mWantedRate = rate;
    mRateRetries = 0;
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "asked the display for %.0f Hz", rate);
    return true;
}

// Horizon OS puts the panel back to its own rate at the end of the launch transition, which lands
// after the one request the game makes. Ask again whenever the rate is not the one asked for, a few
// times, so a runtime that simply refuses the rate is not argued with for the whole run.
void GfxWindowBackendOpenXR::HoldRefreshRate() {
    if (mWantedRate <= 0.0f || mRequestRefreshRate == nullptr || fabsf(mRefreshRate - mWantedRate) < 0.5f ||
        mRateRetries >= 4) {
        return;
    }
    mRateRetries++;
    if (!Failed(mInstance, mRequestRefreshRate(mSession, mWantedRate), "xrRequestDisplayRefreshRateFB")) {
        __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "asked the display for %.0f Hz again", mWantedRate);
    }
}

void GfxWindowBackendOpenXR::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    // SDL reports the panel mode the 2D window was made in, which is not what the compositor runs
    // the headset at once the app asks for a rate.
    if (mActive && mRefreshRate > 0.0f) {
        *refreshRate = (uint32_t)lroundf(mRefreshRate);
        return;
    }
    GfxWindowBackendSDL2::GetActiveWindowRefreshRate(refreshRate);
}

bool GfxWindowBackendOpenXR::StartActions(bool handInteraction) {
    XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(setInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "pointer");
    snprintf(setInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Pointer");
    if (Failed(mInstance, xrCreateActionSet(mInstance, &setInfo, &mActionSet), "xrCreateActionSet")) {
        return false;
    }

    xrStringToPath(mInstance, "/user/hand/left", &mHandPath[0]);
    xrStringToPath(mInstance, "/user/hand/right", &mHandPath[1]);

    XrActionCreateInfo aimInfo{ XR_TYPE_ACTION_CREATE_INFO };
    snprintf(aimInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "aim");
    snprintf(aimInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Aim");
    aimInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    aimInfo.countSubactionPaths = 2;
    aimInfo.subactionPaths = mHandPath;
    if (Failed(mInstance, xrCreateAction(mActionSet, &aimInfo, &mAimAction), "xrCreateAction(aim)")) {
        return false;
    }

    XrActionCreateInfo selectInfo{ XR_TYPE_ACTION_CREATE_INFO };
    snprintf(selectInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "select");
    snprintf(selectInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Select");
    selectInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    selectInfo.countSubactionPaths = 2;
    selectInfo.subactionPaths = mHandPath;
    if (Failed(mInstance, xrCreateAction(mActionSet, &selectInfo, &mSelectAction), "xrCreateAction(select)")) {
        return false;
    }

    auto suggest = [&](const char* profile, const char* leftSelect, const char* rightSelect) {
        XrPath profilePath = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(mInstance, profile, &profilePath))) {
            return;
        }
        XrPath leftAim = XR_NULL_PATH;
        XrPath rightAim = XR_NULL_PATH;
        XrPath leftClick = XR_NULL_PATH;
        XrPath rightClick = XR_NULL_PATH;
        xrStringToPath(mInstance, "/user/hand/left/input/aim/pose", &leftAim);
        xrStringToPath(mInstance, "/user/hand/right/input/aim/pose", &rightAim);
        xrStringToPath(mInstance, leftSelect, &leftClick);
        xrStringToPath(mInstance, rightSelect, &rightClick);

        const XrActionSuggestedBinding bindings[] = {
            { mAimAction, leftAim },
            { mAimAction, rightAim },
            { mSelectAction, leftClick },
            { mSelectAction, rightClick },
        };
        XrInteractionProfileSuggestedBinding suggestion{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggestion.interactionProfile = profilePath;
        suggestion.countSuggestedBindings = 4;
        suggestion.suggestedBindings = bindings;
        xrSuggestInteractionProfileBindings(mInstance, &suggestion);
    };

    suggest("/interaction_profiles/khr/simple_controller", "/user/hand/left/input/select/click",
            "/user/hand/right/input/select/click");
    if (handInteraction) {
        suggest("/interaction_profiles/ext/hand_interaction_ext", "/user/hand/left/input/aim_activate_ext/value",
                "/user/hand/right/input/aim_activate_ext/value");
    }

    if (!StartPadActions()) {
        return false;
    }

    for (int hand = 0; hand < 2; hand++) {
        XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = mAimAction;
        spaceInfo.subactionPath = mHandPath[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (Failed(mInstance, xrCreateActionSpace(mSession, &spaceInfo, &mAimSpace[hand]), "xrCreateActionSpace")) {
            return false;
        }
    }

    XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &mActionSet;
    return !Failed(mInstance, xrAttachSessionActionSets(mSession, &attachInfo), "xrAttachSessionActionSets");
}

static Fast::XrPadState sPad = {};
static bool sPadValid = false;

// The game pad, which on Horizon OS can only come through here: a Touch controller reaches no
// Android input device, so SDL never sees one. The whole Touch profile is suggested in this one
// call, aim and select included, because a second call for the same profile replaces the first.
bool GfxWindowBackendOpenXR::StartPadActions() {
    auto make = [&](const char* name, const char* localized, XrActionType type, XrAction* action) {
        XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
        snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
        snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localized);
        info.actionType = type;
        info.countSubactionPaths = 2;
        info.subactionPaths = mHandPath;
        return !Failed(mInstance, xrCreateAction(mActionSet, &info, action), "xrCreateAction");
    };

    if (!make("stick", "Stick", XR_ACTION_TYPE_VECTOR2F_INPUT, &mStickAction) ||
        !make("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, &mTriggerAction) ||
        !make("squeeze", "Squeeze", XR_ACTION_TYPE_FLOAT_INPUT, &mSqueezeAction) ||
        !make("facelow", "Face Low", XR_ACTION_TYPE_BOOLEAN_INPUT, &mFaceLowAction) ||
        !make("facehigh", "Face High", XR_ACTION_TYPE_BOOLEAN_INPUT, &mFaceHighAction) ||
        !make("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &mMenuAction) ||
        !make("stickclick", "Stick Click", XR_ACTION_TYPE_BOOLEAN_INPUT, &mStickClickAction)) {
        return false;
    }

    XrPath profile = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(mInstance, "/interaction_profiles/oculus/touch_controller", &profile))) {
        return true;
    }

    std::vector<XrActionSuggestedBinding> bindings;
    auto bind = [&](XrAction action, const char* path) {
        XrPath bound = XR_NULL_PATH;
        if (XR_SUCCEEDED(xrStringToPath(mInstance, path, &bound))) {
            bindings.push_back({ action, bound });
        }
    };

    bind(mAimAction, "/user/hand/left/input/aim/pose");
    bind(mAimAction, "/user/hand/right/input/aim/pose");
    bind(mSelectAction, "/user/hand/left/input/trigger/value");
    bind(mSelectAction, "/user/hand/right/input/trigger/value");
    bind(mStickAction, "/user/hand/left/input/thumbstick");
    bind(mStickAction, "/user/hand/right/input/thumbstick");
    bind(mTriggerAction, "/user/hand/left/input/trigger/value");
    bind(mTriggerAction, "/user/hand/right/input/trigger/value");
    bind(mSqueezeAction, "/user/hand/left/input/squeeze/value");
    bind(mSqueezeAction, "/user/hand/right/input/squeeze/value");
    bind(mFaceLowAction, "/user/hand/left/input/x/click");
    bind(mFaceLowAction, "/user/hand/right/input/a/click");
    bind(mFaceHighAction, "/user/hand/left/input/y/click");
    bind(mFaceHighAction, "/user/hand/right/input/b/click");
    // The right hand's system button belongs to Horizon OS and cannot be bound.
    bind(mMenuAction, "/user/hand/left/input/menu/click");
    bind(mStickClickAction, "/user/hand/left/input/thumbstick/click");
    bind(mStickClickAction, "/user/hand/right/input/thumbstick/click");

    XrInteractionProfileSuggestedBinding suggestion{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggestion.interactionProfile = profile;
    suggestion.countSuggestedBindings = (uint32_t)bindings.size();
    suggestion.suggestedBindings = bindings.data();
    xrSuggestInteractionProfileBindings(mInstance, &suggestion);
    return true;
}

void GfxWindowBackendOpenXR::PumpPad() {
    Fast::XrPadState pad = {};
    bool any = false;

    for (int hand = 0; hand < 2; hand++) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.subactionPath = mHandPath[hand];

        info.action = mStickAction;
        XrActionStateVector2f stick{ XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_SUCCEEDED(xrGetActionStateVector2f(mSession, &info, &stick)) && stick.isActive) {
            pad.stick[hand][0] = stick.currentState.x;
            pad.stick[hand][1] = stick.currentState.y;
            any = true;
        }

        XrActionStateFloat value{ XR_TYPE_ACTION_STATE_FLOAT };
        info.action = mTriggerAction;
        if (XR_SUCCEEDED(xrGetActionStateFloat(mSession, &info, &value)) && value.isActive) {
            pad.trigger[hand] = value.currentState;
            any = true;
        }
        value = { XR_TYPE_ACTION_STATE_FLOAT };
        info.action = mSqueezeAction;
        if (XR_SUCCEEDED(xrGetActionStateFloat(mSession, &info, &value)) && value.isActive) {
            pad.squeeze[hand] = value.currentState;
            any = true;
        }

        auto held = [&](XrAction action) {
            info.action = action;
            XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_FAILED(xrGetActionStateBoolean(mSession, &info, &state)) || !state.isActive) {
                return false;
            }
            any = true;
            return state.currentState == XR_TRUE;
        };

        if (held(mFaceLowAction)) {
            pad.buttons |= hand == 0 ? Fast::XR_PAD_X : Fast::XR_PAD_A;
        }
        if (held(mFaceHighAction)) {
            pad.buttons |= hand == 0 ? Fast::XR_PAD_Y : Fast::XR_PAD_B;
        }
        if (held(mMenuAction)) {
            pad.buttons |= Fast::XR_PAD_MENU;
        }
        if (held(mStickClickAction)) {
            pad.buttons |= hand == 0 ? Fast::XR_PAD_LEFT_STICK : Fast::XR_PAD_RIGHT_STICK;
        }
    }

    sPad = pad;
    sPadValid = any;
}

bool GetXrPad(XrPadState* pad) {
    if (!sPadValid || pad == nullptr) {
        return false;
    }
    *pad = sPad;
    return true;
}

static XrVector3f RotateByQuaternion(const XrQuaternionf& q, const XrVector3f& v) {
    const float x = q.y * v.z - q.z * v.y + q.w * v.x;
    const float y = q.z * v.x - q.x * v.z + q.w * v.y;
    const float z = q.x * v.y - q.y * v.x + q.w * v.z;
    return { v.x + 2.0f * (q.y * z - q.z * y), v.y + 2.0f * (q.z * x - q.x * z), v.z + 2.0f * (q.x * y - q.y * x) };
}

static XrVector3f RotateInverse(const XrQuaternionf& q, const XrVector3f& v) {
    return RotateByQuaternion({ -q.x, -q.y, -q.z, q.w }, v);
}

static XrVector3f Subtract(const XrVector3f& a, const XrVector3f& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static float Length(const XrVector3f& v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static bool sPointerDown = false;
static float sPointerU = 0.0f;
static float sPointerV = 0.0f;
static bool sMenuHover = false;
static bool sMenuHeld = false;
// Where the cursor is drawn, in meters from the middle of the window, on the plane it hangs in.
static bool sCursorValid = false;
static float sCursorX = 0.0f;
static float sCursorY = 0.0f;

// Each hand that reaches the window, whether or not it is the one the window answers. A dot alone
// says nothing about which hand put it there, so the ray back to the hand is drawn with it.
static bool sHandValid[2] = { false, false };
static XrVector3f sHandFrom[2] = {};
static float sHandX[2] = { 0.0f, 0.0f };
static float sHandY[2] = { 0.0f, 0.0f };
static bool sHandDown[2] = { false, false };

static void ToggleMenu() {
    auto context = Ship::Context::GetRawInstance();
    if (context == nullptr || context->GetWindow() == nullptr || context->GetWindow()->GetGui() == nullptr) {
        return;
    }
    auto menu = context->GetWindow()->GetGui()->GetMenu();
    if (menu == nullptr) {
        return;
    }
    menu->ToggleVisibility();
    context->GetWindow()->GetMouseStateManager()->UpdateMouseCapture();
}

static bool sViewGeometryValid = false;
static XrViewGeometry sViewGeometry = {};
static float sViewTanHalfWidth = 0.0f;
static float sViewTanHalfHeight = 0.0f;
static float sWindowAngularWidth = 0.0f;
static float sRenderScale = 1.0f;
static bool sFlatProjection = false;
static bool sStereo = true;
static float sEdgeSoftness = 0.36f;
static float sEdgeFloat = 0.15f;
static int sCurrentViewIndex = 0;
static bool sRecenterWanted = true;
static float sSceneNear = std::numeric_limits<float>::max();
static float sGlassDepth = WINDOW_DEPTH_MAX;

bool GetXrViewGeometry(XrViewGeometry* geometry) {
    if (!sViewGeometryValid || sFlatProjection) {
        return false;
    }
    *geometry = sViewGeometry;
    return true;
}

void SetXrViewTangents(float tanHalfWidth, float tanHalfHeight) {
    sViewTanHalfWidth = tanHalfWidth;
    sViewTanHalfHeight = tanHalfHeight;
}

void SetXrSceneNear(float units) {
    if (units < sSceneNear) {
        sSceneNear = units;
    }
}

void RecenterXrWindow() {
    sRecenterWanted = true;
}

void SetXrStereo(bool enabled) {
    sStereo = enabled;
}

void SetXrEdgeSoftness(float softness) {
    sEdgeSoftness = Clamp(softness, 0.0f, EDGE_SOFTNESS_MAX);
}

void SetXrEdgeFloat(float fraction) {
    sEdgeFloat = Clamp(fraction, 0.0f, EDGE_FLOAT_MAX);
}

int GetXrViewIndex() {
    return sCurrentViewIndex;
}

void SetXrFlatProjection(bool flat) {
    sFlatProjection = flat;
}

void SetXrWindowDistance(float meters) {
    sWindowDistance = Clamp(meters, WINDOW_DISTANCE_MIN, WINDOW_DISTANCE_MAX);
}

float GetXrWindowDistance() {
    return sWindowDistance;
}

void SetXrWindowScale(float scale) {
    sWindowScale = Clamp(scale, WINDOW_SCALE_MIN, WINDOW_SCALE_MAX);
}

void SetXrDioramaDepth(float meters) {
    sDioramaDepth = Clamp(meters, DIORAMA_DEPTH_MIN, DIORAMA_DEPTH_MAX);
}

float GetXrWindowScale() {
    return sWindowScale;
}

float GetXrWindowAngularWidth() {
    return sWindowAngularWidth;
}

float GetXrRenderScale() {
    return sRenderScale;
}

XrVector3f GfxWindowBackendOpenXR::HeadPosition() const {
    const XrVector3f& left = mViews[0].pose.position;
    const XrVector3f& right = mViews[1].pose.position;
    return { 0.5f * (left.x + right.x), 0.5f * (left.y + right.y), 0.5f * (left.z + right.z) };
}

// Where an aim ray meets the plane the window hangs in, in meters from the middle of it. The button,
// the move bar and the corner handle are all on that plane, so one intersection answers for them
// all.
bool GfxWindowBackendOpenXR::PlaneHit(const XrPosef& pose, float* planeX, float* planeY) const {
    const XrVector3f origin = ToWindowAxes(pose.position);
    const XrVector3f aim = RotateByQuaternion(pose.orientation, { 0.0f, 0.0f, -1.0f });
    const XrVector3f forward = RotateInverse(mAnchorPose.orientation, aim);
    if (forward.z >= -1e-4f) {
        return false;
    }
    const float t = (-mWindowRadius - origin.z) / forward.z;
    if (t <= 0.0f) {
        return false;
    }
    *planeX = origin.x + t * forward.x;
    *planeY = origin.y + t * forward.y;
    return true;
}

// How far out along the window's diagonal a point on the plane sits. The aspect is locked, so the
// direction of that diagonal does not change with the size and this one number is the whole resize.
float GfxWindowBackendOpenXR::DiagonalReach(float planeX, float planeY) const {
    const float halfWidth = 0.5f * mWindowWidth;
    const float halfHeight = 0.5f * mWindowHeight;
    const float diagonal = sqrtf(halfWidth * halfWidth + halfHeight * halfHeight);
    return diagonal > 0.0f ? (fabsf(planeX) * halfWidth + fabsf(planeY) * halfHeight) / diagonal : 0.0f;
}

float GfxWindowBackendOpenXR::BarWidth() const {
    return mWindowWidth * MOVE_BAR_WIDTH;
}

float GfxWindowBackendOpenXR::BarHeight() const {
    return mWindowHeight * MOVE_BAR_HEIGHT;
}

float GfxWindowBackendOpenXR::BarDrop() const {
    return -mWindowHeight * (0.5f + MOVE_BAR_GAP + MOVE_BAR_HEIGHT * 0.5f);
}

float GfxWindowBackendOpenXR::CornerSide() const {
    return mWindowHeight * CORNER_SIDE;
}

float GfxWindowBackendOpenXR::WindowCorner() const {
    return mWindowHeight * CORNER_SIDE * CORNER_ARC_CENTER;
}

// The side edge one eye gives up, in window heights, which is what the shader measures in.
float GfxWindowBackendOpenXR::EdgeFloat() const {
    if (mViewCount != VIEW_COUNT || !mViewsValid || mWindowHeight <= 0.0f) {
        return 0.0f;
    }
    const float separation = Length(Subtract(mViews[1].pose.position, mViews[0].pose.position));
    return sEdgeFloat * separation / mWindowHeight;
}

bool GfxWindowBackendOpenXR::OnBar(float planeX, float planeY) const {
    return fabsf(planeX) <= 0.625f * BarWidth() && fabsf(planeY - BarDrop()) <= 0.5f * BarHeight() * MOVE_BAR_REACH;
}

// The corner outside the picture, never the picture inside it. The picture is a rounded rectangle,
// so the handle takes the corner the rounding left. Bit 0 is the right side, bit 1 the top.
int GfxWindowBackendOpenXR::OnCorner(float planeX, float planeY) const {
    const float zone = mWindowHeight * CORNER_ZONE;
    const float halfWidth = 0.5f * mWindowWidth;
    const float halfHeight = 0.5f * mWindowHeight;
    const float x = fabsf(planeX);
    const float y = fabsf(planeY);
    const float radius = WindowCorner();
    const float outX = fmaxf(x - halfWidth + radius, 0.0f);
    const float outY = fmaxf(y - halfHeight + radius, 0.0f);
    if (x <= halfWidth - zone || x >= halfWidth + zone || y <= halfHeight - zone || y >= halfHeight + zone ||
        outX * outX + outY * outY <= radius * radius) {
        return -1;
    }
    return (planeX > 0.0f ? 1 : 0) | (planeY > 0.0f ? 2 : 0);
}

// A grab re-bases the window onto the head as it is now, so the range and the size it leaves are
// the ones the user sees from where they stand.
void GfxWindowBackendOpenXR::StartGrab(Grab kind, int hand, const XrVector3f& handPosition, float planeX,
                                       float planeY) {
    const XrVector3f head = HeadPosition();
    const XrVector3f reach = Subtract(mAnchorPose.position, head);
    const float radius = Length(reach);
    mGrabReach = DiagonalReach(planeX, planeY);
    if (radius < 1e-3f || (kind == Grab::Resize && mGrabReach < 1e-3f)) {
        return;
    }
    mGrab = kind;
    mGrabHand = hand;
    mBarHover = kind == Grab::Move;
    if (kind == Grab::Move) {
        mCornerHover = -1;
    }

    // The head has moved since the window was placed, so the range the user pulls against is the
    // one from where they stand now. The size is glass and holds through that, and the window turns
    // to face the user because that is what a window taken hold of does.
    mGrabHandPosition = handPosition;
    mGrabWindowPosition = mAnchorPose.position;
    mGrabScale = mWindowScale;
    mPlacementHead = head;
    mWindowDir = { reach.x / radius, reach.y / radius, reach.z / radius };
    mWindowRadius = radius;
    PlaceWindow();
    sWindowDistance = mWindowRadius;
    sWindowScale = mWindowScale;

    // An anchor is a fixed pose and cannot follow the hand. A new one is made where the pinch lifts.
    if (mAnchorSpace != XR_NULL_HANDLE) {
        xrDestroySpace(mAnchorSpace);
        mAnchorSpace = XR_NULL_HANDLE;
    }
}

// Answers false when the pinch has lifted, which is what leaves the window where the hand left it.
bool GfxWindowBackendOpenXR::UpdateGrab(XrTime displayTime) {
    XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
    getInfo.action = mSelectAction;
    getInfo.subactionPath = mHandPath[mGrabHand];
    XrActionStateBoolean select{ XR_TYPE_ACTION_STATE_BOOLEAN };
    XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_FAILED(xrGetActionStateBoolean(mSession, &getInfo, &select)) || !select.isActive ||
        select.currentState != XR_TRUE ||
        XR_FAILED(xrLocateSpace(mAimSpace[mGrabHand], mSpace, displayTime, &location)) ||
        (location.locationFlags & needed) != needed) {
        return false;
    }

    if (mGrab == Grab::Move) {
        // The window goes where the hand goes, one meter for one meter. The user thus keeps hold of
        // the part of the bar they took, and the window does not jump at the moment of the grab.
        const XrVector3f moved = Subtract(location.pose.position, mGrabHandPosition);
        const XrVector3f from = { mGrabWindowPosition.x + moved.x - mPlacementHead.x,
                                  mGrabWindowPosition.y + moved.y - mPlacementHead.y,
                                  mGrabWindowPosition.z + moved.z - mPlacementHead.z };
        const float radius = Length(from);
        if (radius > 1e-3f) {
            mWindowDir = { from.x / radius, from.y / radius, from.z / radius };
            mWindowRadius = radius;
            PlaceWindow();
            sWindowDistance = mWindowRadius;
            sWindowScale = mWindowScale;
        }
    } else {
        // The plane does not move under a resize, so the corner stays under the ray that took it.
        float planeX = 0.0f;
        float planeY = 0.0f;
        if (PlaneHit(location.pose, &planeX, &planeY)) {
            const float reach = DiagonalReach(planeX, planeY);
            if (reach > 1e-3f) {
                mWindowScale = mGrabScale * reach / mGrabReach;
                PlaceWindow();
                sWindowDistance = mWindowRadius;
                sWindowScale = mWindowScale;
            }
        }
    }

    // The hand is on the handle, and the handle lights to say so. A cursor as well is one mark too
    // many, and under a move it would have to leave the point the user took hold of.
    sCursorValid = false;
    return true;
}

void GfxWindowBackendOpenXR::EndGrab() {
    const Grab kind = mGrab;
    mGrab = Grab::None;
    mBarHover = false;
    mCornerHover = -1;
    AnchorHere();
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "window %s: %.2f x %.2f m at %.2f m, %.2f times its own size, %.1f degrees wide",
                        kind == Grab::Move ? "moved" : "resized", mWindowWidth, mWindowHeight, mWindowRadius,
                        mWindowScale, sWindowAngularWidth * 180.0f / (float)M_PI);
}

void GfxWindowBackendOpenXR::ClearPointer() {
    sMenuHover = false;
    sMenuHeld = false;
    sCursorValid = false;
    sHandValid[0] = false;
    sHandValid[1] = false;
    // A session that stops answering ends the grab where the window stands, anchor and all.
    if (mGrab != Grab::None && mAnchorValid) {
        EndGrab();
    }
    mBarHover = false;
    mCornerHover = -1;
    mGrab = Grab::None;
}

void GfxWindowBackendOpenXR::PumpPointer(XrTime displayTime) {
    if (mActionSet == XR_NULL_HANDLE || mState != XR_SESSION_STATE_FOCUSED) {
        ClearPointer();
        sPadValid = false;
        return;
    }

    XrActiveActionSet active{ mActionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    if (Failed(mInstance, xrSyncActions(mSession, &syncInfo), "xrSyncActions")) {
        return;
    }

    // The pad answers wherever the window is, so it is read before the window is asked about.
    PumpPad();

    if (!mAnchorValid) {
        ClearPointer();
        return;
    }

    // A held handle owns that hand until it lifts, and nothing else on the window answers. The
    // handle lights to say so, and a dot left over the point the hand took would only follow it.
    if (mGrab != Grab::None) {
        if (UpdateGrab(displayTime)) {
            sHandValid[0] = false;
            sHandValid[1] = false;
            return;
        }
        EndGrab();
    }

    bool hit = false;
    bool down = false;
    bool menuHit = false;
    bool menuDown = false;
    bool barHit = false;
    bool barDown = false;
    int cornerHit = -1;
    bool cornerDown = false;
    bool cursor = false;
    float cursorX = 0.0f;
    float cursorY = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    int pinchHand = -1;
    XrVector3f pinchPosition = {};
    float pinchX = 0.0f;
    float pinchY = 0.0f;
    bool answered = false;

    sHandValid[0] = false;
    sHandValid[1] = false;

    const float menuHalf = 0.5f * MenuSide();
    const float zoneHalf = 0.5f * MenuZone();
    const float menuRise = MenuRise();

    // Both hands are read before any of them is answered, because both are drawn: each one that
    // reaches the window carries a dot, and a ray back to the hand it came from.
    struct HandAim {
        bool onPlane;
        XrVector3f from;
        float planeX;
        float planeY;
        bool pinching;
    } aims[2] = {};

    for (int hand = 0; hand < 2; hand++) {
        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace(mAimSpace[hand], mSpace, displayTime, &location)) ||
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 ||
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
            continue;
        }
        if (!PlaneHit(location.pose, &aims[hand].planeX, &aims[hand].planeY)) {
            continue;
        }

        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = mSelectAction;
        getInfo.subactionPath = mHandPath[hand];
        XrActionStateBoolean select{ XR_TYPE_ACTION_STATE_BOOLEAN };
        aims[hand].onPlane = true;
        aims[hand].from = location.pose.position;
        aims[hand].pinching = XR_SUCCEEDED(xrGetActionStateBoolean(mSession, &getInfo, &select)) && select.isActive &&
                              select.currentState == XR_TRUE;
    }

    for (int hand = 0; hand < 2; hand++) {
        if (!aims[hand].onPlane) {
            continue;
        }
        const float planeX = aims[hand].planeX;
        const float planeY = aims[hand].planeY;
        const bool pinching = aims[hand].pinching;

        const bool onMenu = fabsf(planeX) <= menuHalf && fabsf(planeY - menuRise) <= menuHalf;
        const bool onWindow = fabsf(planeX) <= 0.5f * mWindowWidth && fabsf(planeY) <= 0.5f * mWindowHeight;
        const bool inZone = fabsf(planeX) <= zoneHalf && fabsf(planeY - menuRise) <= zoneHalf;
        const bool onBar = OnBar(planeX, planeY);
        const int onCorner = OnCorner(planeX, planeY);
        if (!onWindow && !inZone && !onBar && onCorner < 0) {
            continue;
        }

        // Both hands are drawn, so both are marked here. Only one of them answers: a pinch takes
        // the window and the hand that took it keeps it, which is what answered says.
        sHandValid[hand] = true;
        sHandFrom[hand] = aims[hand].from;
        sHandX[hand] = planeX;
        sHandY[hand] = planeY;
        sHandDown[hand] = pinching;
        if (answered) {
            continue;
        }

        // A pinching hand wins; otherwise the first hand on the quad drives the cursor.
        if (!cursor || pinching) {
            cursor = true;
            cursorX = planeX;
            cursorY = planeY;
        }
        if (onMenu) {
            if (!menuHit || pinching) {
                menuHit = true;
                menuDown = pinching;
            }
        } else if (onBar) {
            if (!barHit || pinching) {
                barHit = true;
                barDown = pinching;
            }
        } else if (onCorner >= 0) {
            if (cornerHit < 0 || pinching) {
                cornerHit = onCorner;
                cornerDown = pinching;
            }
        } else if (onWindow && (!hit || pinching)) {
            hit = true;
            down = pinching;
            u = planeX / mWindowWidth + 0.5f;
            v = 0.5f - planeY / mWindowHeight;
        }
        if (pinching) {
            pinchHand = hand;
            pinchPosition = aims[hand].from;
            pinchX = planeX;
            pinchY = planeY;
            answered = true;
        }
    }

#ifdef ENABLE_DEBUG_TOOLS
    // A pointer put on the window from the host, so a prompt can be answered with no hand in the
    // headset. It takes the place of a hand on the picture and leaves the handles alone.
    float debugU = 0.0f;
    float debugV = 0.0f;
    bool debugDown = false;
    if (DebugPointer::Poll(&debugU, &debugV, &debugDown)) {
        hit = true;
        down = debugDown;
        u = debugU;
        v = debugV;
        cursor = true;
        cursorX = (u - 0.5f) * mWindowWidth;
        cursorY = (0.5f - v) * mWindowHeight;

        // A hand is invented for it, part of the way to the window and low and to the right, so
        // that what a hand draws can be looked at from the host as well.
        if (mViewsValid) {
            const XrVector3f head = HeadPosition();
            const XrVector3f right = RotateByQuaternion(mAnchorPose.orientation, { 1.0f, 0.0f, 0.0f });
            const XrVector3f up = RotateByQuaternion(mAnchorPose.orientation, { 0.0f, 1.0f, 0.0f });
            const XrVector3f reach = Subtract(mAnchorPose.position, head);
            sHandValid[0] = true;
            sHandDown[0] = debugDown;
            sHandX[0] = cursorX;
            sHandY[0] = cursorY;
            sHandFrom[0] = { head.x + reach.x * 0.35f + right.x * 0.20f - up.x * 0.25f,
                             head.y + reach.y * 0.35f + right.y * 0.20f - up.y * 0.25f,
                             head.z + reach.z * 0.35f + right.z * 0.20f - up.z * 0.25f };
        }
    }
#endif

    const bool wasDown = sPointerDown;
    mBarHover = barHit;
    mCornerHover = cornerHit;

    // A pinch on a handle takes the window, but only if it did not start on the picture: a finger
    // already down belongs to the game until it lifts.
    if (!wasDown && !sMenuHeld && mViewsValid && pinchHand >= 0 && (barDown || cornerDown)) {
        StartGrab(barDown ? Grab::Move : Grab::Resize, pinchHand, pinchPosition, pinchX, pinchY);
        if (mGrab != Grab::None) {
            sMenuHover = false;
            sCursorValid = true;
            sCursorX = cursorX;
            sCursorY = cursorY;
            sHandValid[0] = false;
            sHandValid[1] = false;
            return;
        }
    }

    // A pinch that lands on the button holds it until it lifts. It works on the lift, so a pinch
    // that leaves the button before it opens does nothing, and one that arrives from the picture
    // already closed never takes it.
    const bool holding = menuHit && menuDown;
    if (sMenuHeld && !holding) {
        if (menuHit) {
            ToggleMenu();
        }
        sMenuHeld = false;
    } else if (holding && !wasDown) {
        sMenuHeld = true;
    }
    sMenuHover = menuHit;

    if (cursor) {
        sCursorValid = true;
        sCursorX = cursorX;
        sCursorY = cursorY;
    } else if (!wasDown) {
        sCursorValid = false;
    }

    if (hit) {
        sPointerU = u;
        sPointerV = v;
    }

    if (!hit && !wasDown) {
        return;
    }

    // The pad reads SDL's touch device list, which SDL_PushEvent cannot reach, so the pinch goes
    // in there directly. ImGui reads mouse events, which is what SDL synthesizes from a touch on a
    // phone, so the menu is driven the same way here.
    const int x = (int)(sPointerU * (float)mGameWidth);
    const int y = (int)(sPointerV * (float)mGameHeight);

    // ImGui's SDL backend drops an event whose window it does not know, and a pushed event does
    // not carry one by itself.
    const Uint32 windowId = SDL_GetWindowID(SDL_GL_GetCurrentWindow());

    SDL_Event motion{};
    motion.type = SDL_MOUSEMOTION;
    motion.motion.windowID = windowId;
    motion.motion.timestamp = SDL_GetTicks();
    motion.motion.state = down ? SDL_BUTTON_LMASK : 0;
    motion.motion.x = x;
    motion.motion.y = y;
    SDL_PushEvent(&motion);

    if (down != wasDown) {
        SDL_Event button{};
        button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        button.button.windowID = windowId;
        button.button.timestamp = SDL_GetTicks();
        button.button.button = SDL_BUTTON_LEFT;
        button.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        button.button.clicks = 1;
        button.button.x = x;
        button.button.y = y;
        SDL_PushEvent(&button);
    }
    sPointerDown = down;
}

void GfxWindowBackendOpenXR::PollEvents() {
    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    while (true) {
        event = { XR_TYPE_EVENT_DATA_BUFFER };
        const XrResult result = xrPollEvent(mInstance, &event);
        if (result != XR_SUCCESS) {
            return;
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            HandleStateChange(*(const XrEventDataSessionStateChanged*)&event);
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            mActive = false;
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            HandleReferenceSpaceChange(*(const XrEventDataReferenceSpaceChangePending*)&event);
        } else if (event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            // The runtime can refuse or later drop the rate the game asked for, and the sub-frame
            // count follows whatever it reports here.
            mRefreshRate = ((const XrEventDataDisplayRefreshRateChangedFB*)&event)->toDisplayRefreshRate;
            __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "display now runs at %.0f Hz", mRefreshRate);
            HoldRefreshRate();
        }
    }
}

// The recenter gesture re-origins LOCAL, not the space the window hangs in. Android XR also
// re-origins LOCAL by itself to keep it near the user, and the window must not chase that; only
// the gesture turns the origin to face where the user faces.
void GfxWindowBackendOpenXR::HandleReferenceSpaceChange(const XrEventDataReferenceSpaceChangePending& change) {
    // Without an anchor the window pose is held in the coordinates of its own space, so a re-origin
    // of that space carries the window away with it. An anchor holds the window through one.
    bool wanted = change.referenceSpaceType == mSpaceType && mAnchorSpace == XR_NULL_HANDLE;
    float turn = 0.0f;
    if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL && change.poseValid == XR_TRUE) {
        turn = YawOf(change.poseInPreviousSpace.orientation);
        wanted = wanted || fabsf(turn) > RECENTER_YAW_MIN;
    }
    if (!wanted) {
        return;
    }

    sRecenterWanted = true;
    // The new origin only takes effect for a locate at or after this time, so a recenter run any
    // earlier would place the window with poses read in the old one.
    mRecenterAfter = change.changeTime;
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "space %d turned %.0f degrees, window re-fronted",
                        (int)change.referenceSpaceType, turn * 180.0f / (float)M_PI);
}

// Android XR announces a re-origin of a space only while the application locates that space, so
// this call is what carries the recenter gesture. Nothing reads the result.
void GfxWindowBackendOpenXR::PollLocalSpace() {
    if (mLocalSpace == XR_NULL_HANDLE) {
        return;
    }
    XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
    xrLocateSpace(mLocalSpace, mSpace, mDisplayTime, &location);
}

void GfxWindowBackendOpenXR::HandleStateChange(const XrEventDataSessionStateChanged& changed) {
    mState = changed.state;
    SPDLOG_INFO("OpenXR: session state {}", (int)mState);

    if (mState == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        mRunning = !Failed(mInstance, xrBeginSession(mSession, &beginInfo), "xrBeginSession");
    } else if (mState == XR_SESSION_STATE_FOCUSED) {
        mRateRetries = 0;
        HoldRefreshRate();
    } else if (mState == XR_SESSION_STATE_STOPPING) {
        Failed(mInstance, xrEndSession(mSession), "xrEndSession");
        mRunning = false;
    } else if (mState == XR_SESSION_STATE_EXITING || mState == XR_SESSION_STATE_LOSS_PENDING) {
        mRunning = false;
        mActive = false;
    }
}

void GfxWindowBackendOpenXR::LocateViews() {
    mViewsValid = false;

    XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = mDisplayTime;
    locateInfo.space = mSpace;

    XrViewState viewState{ XR_TYPE_VIEW_STATE };
    uint32_t count = 0;
    if (Failed(mInstance, xrLocateViews(mSession, &locateInfo, &viewState, VIEW_COUNT, &count, mViews),
               "xrLocateViews")) {
        return;
    }
    mViewsValid = count == VIEW_COUNT && (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
}

// Where the window hangs comes from four numbers: the head it was placed around, the direction from
// that head to its middle, the range, and what the corner handles left. The range alone puts the
// apex of the game's frustum behind the glass, which holds the framing at every range: the picture
// is the one the game drew, and the range says how large the diorama behind it is in the room. The
// size is glass only. A larger window shows the same diorama over more of the eye, and a further
// range shows a larger one over less of it.
//
// The window hangs on a capsule around the user, not a sphere. It faces the user in the horizontal
// and stands upright through the middle of the range, so it can go up and down with no tilt at all.
// It bends in only near the ends, where an upright panel is read at too flat an angle. The bend
// stops short of straight up, because the yaw of a window that faces the user has no answer there.
// The window takes the shape of the field of view it shows, and the size scale gives it its width
// in meters. That field of view is the game's, once the game has loaded a projection to ask for
// one. Before then —
// the extractor, the ROM prompt, the menu over them — nothing has asked, and the window takes the
// shape of the picture at a plain angle. It cannot take the shape of the swapchain: an eye image is
// about as tall as it is wide, and a landscape picture drawn on a rectangle that shape is squeezed
// to about half its width.
// The game is drawn once per eye and then sampled onto a window that covers part of the view, so
// every pixel it draws past what that window covers is thrown away. The waste is small where the
// headset hands the app a panel the size of one eye and large where it hands it the whole binocular
// panel: Quest gives the game 4128 pixels across for a window that covers about a thousand of them,
// which is sixteen times the pixels and the same multiple of the shading.
//
// The step is coarse so that a hand on a corner handle does not rebuild the targets every frame,
// and the headroom is for a head that leans in towards the window, which widens it in the view.
void GfxWindowBackendOpenXR::SizeRender() {
    if (!mViewsValid || mSwapchainWidth == 0 || mGameWidth == 0 || sWindowAngularWidth <= 0.0f) {
        return;
    }
    const float eyeAngle = mViews[0].fov.angleRight - mViews[0].fov.angleLeft;
    if (eyeAngle <= 0.0f) {
        return;
    }

    const float covered = (float)mSwapchainWidth * sWindowAngularWidth / eyeAngle;
    const float wanted = Clamp(roundf(covered * RENDER_HEADROOM / (float)mGameWidth * RENDER_STEPS) / RENDER_STEPS,
                               1.0f / RENDER_STEPS, 1.0f);
    if (wanted == sRenderScale) {
        return;
    }

    sRenderScale = wanted;
    CreateGameTargets((uint32_t)lroundf((float)mGameWidth * wanted), (uint32_t)lroundf((float)mGameHeight * wanted));
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "the game draws %ux%u for a window %.0f pixels across",
                        mTexWidth, mTexHeight, covered);
}

void GfxWindowBackendOpenXR::SizeWindow() {
    mWindowSized = sViewTanHalfWidth > 0.0f && sViewTanHalfHeight > 0.0f;

    float tanHalfWidth = sViewTanHalfWidth;
    float tanHalfHeight = sViewTanHalfHeight;
    if (!mWindowSized) {
        tanHalfWidth = WINDOW_TAN_HALF_WIDTH_DEFAULT;
        tanHalfHeight = mGameWidth > 0 ? tanHalfWidth * (float)mGameHeight / (float)mGameWidth : tanHalfWidth;
    }

    const float glass = 2.0f * WINDOW_SIZE_RANGE * mWindowScale;
    mWindowWidth = glass * tanHalfWidth;
    mWindowHeight = glass * tanHalfHeight;
}

void GfxWindowBackendOpenXR::PlaceWindow() {
    mWindowRadius = Clamp(mWindowRadius, WINDOW_DISTANCE_MIN, WINDOW_DISTANCE_MAX);
    mWindowScale = Clamp(mWindowScale, WINDOW_SCALE_MIN, WINDOW_SCALE_MAX);
    SizeWindow();
    sWindowAngularWidth = 2.0f * atanf(0.5f * mWindowWidth / mWindowRadius);

    const float across = sqrtf(mWindowDir.x * mWindowDir.x + mWindowDir.z * mWindowDir.z);
    const float azimuth = across > 1e-4f ? atan2f(mWindowDir.x, mWindowDir.z) : 0.0f;
    const float rise = Clamp(atan2f(mWindowDir.y, across), -WINDOW_RISE_MAX, WINDOW_RISE_MAX);
    mWindowDir = { sinf(azimuth) * cosf(rise), sinf(rise), cosf(azimuth) * cosf(rise) };

    // Yaw and pitch. A window that took a roll as well would hang askew in the room.
    const float pitch = rise * Smoothstep(WINDOW_RISE_FLAT, WINDOW_RISE_MAX, fabsf(rise));
    const float yaw = atan2f(-mWindowDir.x, -mWindowDir.z);
    const float cy = cosf(0.5f * yaw);
    const float sy = sinf(0.5f * yaw);
    const float cp = cosf(0.5f * pitch);
    const float sp = sinf(0.5f * pitch);
    mAnchorPose.orientation = { cy * sp, sy * cp, -sy * sp, cy * cp };
    mAnchorPose.position = { mPlacementHead.x + mWindowDir.x * mWindowRadius,
                             mPlacementHead.y + mWindowDir.y * mWindowRadius,
                             mPlacementHead.z + mWindowDir.z * mWindowRadius };

    // Along the window's own normal, which is not the line to the head once the window stands
    // upright below or above it. The apex of the frustum has to sit square behind the glass.
    const XrVector3f normal = RotateByQuaternion(mAnchorPose.orientation, { 0.0f, 0.0f, 1.0f });
    mViewpoint = { mAnchorPose.position.x + normal.x * mWindowRadius,
                   mAnchorPose.position.y + normal.y * mWindowRadius,
                   mAnchorPose.position.z + normal.z * mWindowRadius };
    mAnchorValid = true;
}

// A spatial anchor holds a pose through SLAM itself, so the window stays put even when the
// reference spaces re-origin around the user.
void GfxWindowBackendOpenXR::AnchorHere() {
    if (mCreateAnchorSpace != nullptr) {
        if (mAnchorSpace != XR_NULL_HANDLE) {
            xrDestroySpace(mAnchorSpace);
            mAnchorSpace = XR_NULL_HANDLE;
        }
        XrAnchorSpaceCreateInfoANDROID anchorInfo{ XR_TYPE_ANCHOR_SPACE_CREATE_INFO_ANDROID };
        anchorInfo.space = mSpace;
        anchorInfo.time = mDisplayTime;
        anchorInfo.pose = mAnchorPose;
        anchorInfo.trackable = XR_NULL_TRACKABLE_ANDROID;
        const XrResult result = mCreateAnchorSpace(mSession, &anchorInfo, &mAnchorSpace);
        if (XR_FAILED(result)) {
            mAnchorSpace = XR_NULL_HANDLE;
            __android_log_print(ANDROID_LOG_WARN, "LighthouseXR", "anchor creation failed (%d)", (int)result);
        }
    }
}

// Puts the window where the user is looking now, upright and level. Nothing else ever placed it:
// it used to hang off the origin of the reference space, which is wherever the headset happened to
// be when the session opened.
void GfxWindowBackendOpenXR::Recenter() {
    // The head pose comes from the views already located this frame. Locating VIEW space instead
    // reports no valid pose on Galaxy XR while the headset is worn, which silently disabled every
    // recenter.
    const float yaw = YawOf(mViews[0].pose.orientation);
    // A recenter wins over a drag: it is the way back from a window put where no hand can reach it.
    mGrab = Grab::None;
    mBarHover = false;
    mCornerHover = -1;
    mPlacementHead = HeadPosition();
    mWindowDir = { -sinf(yaw), 0.0f, -cosf(yaw) };
    mWindowRadius = sWindowDistance;
    mWindowScale = sWindowScale;
    sRecenterWanted = false;
    PlaceWindow();
    AnchorHere();

    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "window placed at %.2f %.2f %.2f facing %.0f degrees%s, %.2f x %.2f m at %.2f m, %s",
                        mAnchorPose.position.x, mAnchorPose.position.y, mAnchorPose.position.z,
                        yaw * 180.0f / (float)M_PI, mAnchorSpace != XR_NULL_HANDLE ? " on an anchor" : "", mWindowWidth,
                        mWindowHeight, mWindowRadius,
                        mWindowSized ? "the game's own field of view" : "the shape of the picture");
}

// A point measured from the viewpoint the window was placed for, in the window's own axes.
XrVector3f GfxWindowBackendOpenXR::ToWindowAxes(const XrVector3f& point) const {
    return RotateInverse(mAnchorPose.orientation, Subtract(point, mViewpoint));
}

// In at once, so nothing is ever left standing in front of the window; out slowly, so one near
// object in one frame does not throw the world back and hold it there.
void GfxWindowBackendOpenXR::MoveGlass() {
    float target = sSceneNear * WINDOW_DEPTH_MARGIN;
    if (target > WINDOW_DEPTH_MAX) {
        target = WINDOW_DEPTH_MAX;
    } else if (target < WINDOW_DEPTH_MIN) {
        target = WINDOW_DEPTH_MIN;
    }

    if (target < sGlassDepth) {
        sGlassDepth = target;
    } else {
        const float rate = mRefreshRate > 0.0f ? mRefreshRate : 60.0f;
        sGlassDepth += (target - sGlassDepth) * (1.0f - expf(-1.0f / (rate * WINDOW_DEPTH_RELEASE)));
    }
    sSceneNear = std::numeric_limits<float>::max();
}

// The settings and the handles write the same two numbers, and the handles write them through
// PlaceWindow, so anything that differs here came from the menu and moves the window in place.
void GfxWindowBackendOpenXR::ApplySettings() {
    const bool ranged = sWindowDistance != mWindowRadius || sWindowScale != mWindowScale;
    if (!ranged && (mWindowSized || sViewTanHalfWidth <= 0.0f)) {
        return;
    }
    mWindowRadius = sWindowDistance;
    mWindowScale = sWindowScale;
    PlaceWindow();
    if (ranged) {
        AnchorHere();
    }
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "window %.2f x %.2f m at %.2f m, %.1f degrees wide, the world within %.2f m behind it",
                        mWindowWidth, mWindowHeight, mWindowRadius, sWindowAngularWidth * 180.0f / (float)M_PI,
                        sDioramaDepth);
}

// The rectangle the button is drawn and aimed at, the zone around it the cursor shows in, the
// cursor itself, and how far the button sits above the middle of the window. All follow the window,
// so they keep their size and place as the window resizes.
float GfxWindowBackendOpenXR::MenuSide() const {
    return mWindowHeight * MENU_BUTTON_SIDE * MENU_BUTTON_REACH;
}

float GfxWindowBackendOpenXR::MenuZone() const {
    return mWindowHeight * MENU_BUTTON_SIDE * MENU_BUTTON_ZONE;
}

float GfxWindowBackendOpenXR::CursorSide() const {
    return mWindowHeight * CURSOR_SIDE;
}

float GfxWindowBackendOpenXR::MenuRise() const {
    return mWindowHeight * (0.5f + MENU_BUTTON_GAP + MENU_BUTTON_SIDE * 0.5f);
}

// A pose on the window plane, that many meters right of and above the middle of the window.
XrPosef GfxWindowBackendOpenXR::PlanePose(float x, float y) const {
    const XrVector3f right = RotateByQuaternion(mAnchorPose.orientation, { 1.0f, 0.0f, 0.0f });
    const XrVector3f up = RotateByQuaternion(mAnchorPose.orientation, { 0.0f, 1.0f, 0.0f });
    XrPosef pose = mAnchorPose;
    pose.position = { pose.position.x + right.x * x + up.x * y, pose.position.y + right.y * x + up.y * y,
                      pose.position.z + right.z * x + up.z * y };
    return pose;
}

bool GfxWindowBackendOpenXR::OpenFrame() {
    sViewGeometryValid = false;
    if (!mActive) {
        return false;
    }
#ifdef ENABLE_DEBUG_TOOLS
    // The frame writes the picture and then one image an eye, so the request has to be taken up
    // here rather than by whichever of them asks first.
    DebugCapture::Arm();
#endif

    PollEvents();
    if (!mRunning) {
        return false;
    }

    MoveGlass();

    XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    if (Failed(mInstance, xrWaitFrame(mSession, &waitInfo, &frameState), "xrWaitFrame")) {
        return false;
    }

    XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
    if (Failed(mInstance, xrBeginFrame(mSession, &beginInfo), "xrBeginFrame")) {
        return false;
    }

    mFrameOpen = true;
    mDisplayTime = frameState.predictedDisplayTime;
    mShouldRender = frameState.shouldRender == XR_TRUE;

    LocateViews();
    PollLocalSpace();
    if (mViewsValid && (!mAnchorValid || (sRecenterWanted && mDisplayTime >= mRecenterAfter))) {
        Recenter();
    } else if (mAnchorValid && mGrab == Grab::None) {
        ApplySettings();
    }

    // The anchor is the ground truth for where the window hangs. Follow it in the app space so
    // the off-axis frustum and the pointer agree with the picture the compositor shows.
    if (mAnchorSpace != XR_NULL_HANDLE && mAnchorValid && mGrab == Grab::None) {
        XrSpaceLocation anchor{ XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags needed =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(mAnchorSpace, mSpace, mDisplayTime, &anchor)) &&
            (anchor.locationFlags & needed) == needed) {
            mAnchorPose = anchor.pose;
            const XrVector3f normal = RotateByQuaternion(anchor.pose.orientation, { 0.0f, 0.0f, 1.0f });
            mViewpoint = { anchor.pose.position.x + normal.x * mWindowRadius,
                           anchor.pose.position.y + normal.y * mWindowRadius,
                           anchor.pose.position.z + normal.z * mWindowRadius };
            // The head the window was placed around goes with the anchor. It is not the reverse of
            // the normal any more: an upright window above or below the eyes does not face them.
            mPlacementHead = { anchor.pose.position.x - mWindowDir.x * mWindowRadius,
                               anchor.pose.position.y - mWindowDir.y * mWindowRadius,
                               anchor.pose.position.z - mWindowDir.z * mWindowRadius };
        }
    }

    PumpPointer(mDisplayTime);
    SizeRender();

    // Without a head pose there is no off-axis frustum to build, so the frame falls back to one
    // image that both eyes read, which is what 2.1 presented.
    mViewCount = (mViewsValid && sStereo) ? VIEW_COUNT : 1;
    mCurrentView = 0;
    return true;
}

uint32_t GfxWindowBackendOpenXR::BeginRenderFrame() {
    return OpenFrame() ? mViewCount : 1;
}

void GfxWindowBackendOpenXR::BeginRenderView(uint32_t view) {
    mCurrentView = view;
    sCurrentViewIndex = (int)view;
    sViewGeometryValid = false;
    if (!mFrameOpen || !mViewsValid || !mAnchorValid || view >= VIEW_COUNT) {
        return;
    }

    // Across the glass, the eye offset converts against the glass itself — sGlassDepth game units
    // over a window WINDOW_SIZE_RANGE * scale meters wide — so a point on the glass lands on the
    // same spot for both eyes at any size and range. The gain then sets how deep the world reads:
    // it compresses every disparity so the farthest thing the game draws sits sDioramaDepth meters
    // behind the window, whatever the range and however the window is resized. Along the normal the
    // old scale stands, so the apex of the frustum reaches the glass when the nose does and not a
    // step before; the parallax a lean can put on the glass stays under the same gain.
    const float gain = sDioramaDepth / (mWindowRadius + sDioramaDepth);
    const float acrossGlass = gain * sGlassDepth / (WINDOW_SIZE_RANGE * mWindowScale);
    const float alongNormal = sGlassDepth / mWindowRadius;
    const XrVector3f& left = mViews[0].pose.position;
    const XrVector3f& right = mViews[1].pose.position;
    // One image for both eyes is drawn from between them, not from either one.
    const bool mono = mViewCount != VIEW_COUNT;
    const XrVector3f& eye = mViews[view].pose.position;
    const XrVector3f world =
        mono ? XrVector3f{ 0.5f * (left.x + right.x), 0.5f * (left.y + right.y), 0.5f * (left.z + right.z) } : eye;
    const XrVector3f offset = ToWindowAxes(world);
    sViewGeometry.eyeOffset[0] = offset.x * acrossGlass;
    sViewGeometry.eyeOffset[1] = offset.y * acrossGlass;
    sViewGeometry.eyeOffset[2] = offset.z * alongNormal;
    sViewGeometry.windowDistance = sGlassDepth;
    sViewGeometryValid = true;
}

static GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[512] = "";
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        SPDLOG_ERROR("OpenXR: placement shader failed: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint LinkProgram(GLuint vertex, const char* fragmentSource) {
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0) {
        return 0;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        SPDLOG_ERROR("OpenXR: placement program failed to link");
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool GfxWindowBackendOpenXR::StartPlacementPass() {
    static const char* VERTEX =
        "#version 300 es\n"
        "uniform mat4 uMvp;\n"
        "out vec2 vUv;\n"
        "void main() {\n"
        "    vec2 c = vec2((gl_VertexID & 1) == 1 ? 0.5 : -0.5, (gl_VertexID & 2) == 2 ? 0.5 : -0.5);\n"
        "    vUv = c + 0.5;\n"
        "    gl_Position = uMvp * vec4(c, 0.0, 1.0);\n"
        "}\n";
    // The picture is a rounded rectangle that can fade out at its edge. The shape is measured in
    // window heights, so the rounding and the fade hold their size on the glass whatever the window
    // does. uShift takes this eye's own side edge in and leaves the other, which floats the frame:
    // the same crop off each eye keeps the picture one width and moves only which side gives it up.
    // The ramp is never narrower than a pixel, so the rounding stays clean with the fade at zero.
    // The color comes out multiplied by the alpha, as the layer reads it that way.
    static const char* FRAGMENT = "#version 300 es\n"
                                  "precision highp float;\n"
                                  "uniform sampler2D uTex;\n"
                                  "uniform float uAspect;\n"
                                  "uniform float uRadius;\n"
                                  "uniform float uFeather;\n"
                                  "uniform float uShift;\n"
                                  "in vec2 vUv;\n"
                                  "out vec4 oColor;\n"
                                  "void main() {\n"
                                  "    vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0) - vec2(0.5 * uShift, 0.0);\n"
                                  "    vec2 corner = vec2(0.5 * uAspect - 0.5 * abs(uShift), 0.5);\n"
                                  "    vec2 d = abs(p) - corner + uRadius;\n"
                                  "    float sd = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - uRadius;\n"
                                  "    float a = smoothstep(0.0, max(uFeather, fwidth(sd)), -sd);\n"
                                  "    oColor = vec4(texture(uTex, vUv).rgb * a, a);\n"
                                  "}\n";
    // Three bars in a rounded square, drawn from the distance to each shape so the edges stay clean
    // at any range. SIDE is 1 / MENU_BUTTON_REACH: the rest of the rectangle keeps its alpha at
    // zero and is the part of the target the user cannot see. The color comes out multiplied by
    // the alpha, as the layer and the blend both read it.
    static const char* MENU_FRAGMENT =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform float uGlow;\n"
        "in vec2 vUv;\n"
        "out vec4 oColor;\n"
        "const float SIDE = 0.625;\n"
        "float Box(vec2 p, vec2 corner, float radius) {\n"
        "    vec2 d = abs(p) - corner + radius;\n"
        "    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;\n"
        "}\n"
        "float Cover(float d) {\n"
        "    float aa = fwidth(d);\n"
        "    return 1.0 - smoothstep(-aa, aa, d);\n"
        "}\n"
        "void main() {\n"
        "    vec2 p = vUv - 0.5;\n"
        "    float body = Box(p, vec2(SIDE * 0.5), SIDE * 0.3);\n"
        "    float fill = Cover(body);\n"
        "    float ring = fill - Cover(body + SIDE * 0.045);\n"
        "    float bars = 0.0;\n"
        "    for (int i = -1; i <= 1; i++) {\n"
        "        bars = max(bars, Cover(Box(p - vec2(0.0, float(i) * SIDE * 0.22),\n"
        "                                   vec2(SIDE * 0.25, SIDE * 0.037), SIDE * 0.037)));\n"
        "    }\n"
        "    float alpha = mix(mix(fill * 0.22, 0.55, ring), 0.85, bars);\n"
        "    alpha = min(alpha * uGlow, 1.0);\n"
        "    oColor = vec4(vec3(alpha), alpha);\n"
        "}\n";
    // A ring with a soft dark edge outside it, which is what makes the cursor hold up on a bright
    // sky and on a black cave alike. The shadow stops at the ring: under it, it shows through the
    // open middle and turns the white grey.
    static const char* CURSOR_FRAGMENT =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform float uDown;\n"
        "uniform vec3 uTint;\n"
        "in vec2 vUv;\n"
        "out vec4 oColor;\n"
        "void main() {\n"
        "    vec2 p = vUv - 0.5;\n"
        "    float r = mix(0.32, 0.267, uDown);\n"
        "    float t = 0.053;\n"
        "    float d = length(p);\n"
        "    float aa = fwidth(d);\n"
        "    float disc = 1.0 - smoothstep(r - aa, r + aa, d);\n"
        "    float inner = 1.0 - smoothstep(r - t - aa, r - t + aa, d);\n"
        "    float shadow = (1.0 - smoothstep(r - 0.005, r + 0.05, d)) * (1.0 - disc);\n"
        "    float fillA = inner * 0.35;\n"
        "    float ringA = (disc - inner) * 0.85;\n"
        "    vec4 c = vec4(0.0, 0.0, 0.0, shadow * 0.55);\n"
        "    c = vec4(uTint * fillA, fillA) + c * (1.0 - fillA);\n"
        "    c = vec4(uTint * ringA, ringA) + c * (1.0 - ringA);\n"
        "    oColor = c;\n"
        "}\n";

    // The ray's band is one meter across and the width is set here, from what it should be at each
    // end. Both ends are held at the same angle, so the far one is the wider, and no matrix makes
    // that shape.
    static const char* RAY_VERTEX =
        "#version 300 es\n"
        "uniform mat4 uMvp;\n"
        "uniform vec2 uWidth;\n"
        "out vec2 vUv;\n"
        "void main() {\n"
        "    vec2 c = vec2((gl_VertexID & 1) == 1 ? 0.5 : -0.5, (gl_VertexID & 2) == 2 ? 0.5 : -0.5);\n"
        "    vUv = c + 0.5;\n"
        "    gl_Position = uMvp * vec4(c.x, c.y * mix(uWidth.x, uWidth.y, vUv.x), 0.0, 1.0);\n"
        "}\n";

    // The ray. uFade carries its profile in meters from the hand: nothing until x, full by y, held
    // to z, gone by w. It comes out of nothing rather than starting at the hand, so no controller
    // wears a line on its front, and it is gone before the dot rather than reaching the middle of
    // it. Nothing tapers: the width is an angle and the ramps are the only thing that changes.
    static const char* RAY_FRAGMENT = "#version 300 es\n"
                                      "precision highp float;\n"
                                      "uniform vec3 uTint;\n"
                                      "uniform vec4 uFade;\n"
                                      "uniform vec2 uTaper;\n"
                                      "in vec2 vUv;\n"
                                      "out vec4 oColor;\n"
                                      "void main() {\n"
                                      "    float across = abs(vUv.y - 0.5) * 2.0;\n"
                                      "    float along = vUv.x * uFade.w;\n"
                                      "    float narrow = mix(1.0, uTaper.y, smoothstep(uTaper.x, uFade.w, along));\n"
                                      "    float aa = max(fwidth(across), 0.001);\n"
                                      "    float core = 1.0 - smoothstep(narrow - aa, narrow + aa, across);\n"
                                      "    float a = core * smoothstep(uFade.x, uFade.y, along) *\n"
                                      "              (1.0 - smoothstep(uFade.z, uFade.w, along)) * 0.85;\n"
                                      "    oColor = vec4(uTint * a, a);\n"
                                      "}\n";

    // The move bar. The quad is far wider than it is tall, so the shape is drawn in a space the
    // width stretches, or the ends would be ellipses rather than half circles.
    static const char* BAR_FRAGMENT = "#version 300 es\n"
                                      "precision highp float;\n"
                                      "uniform float uGlow;\n"
                                      "uniform float uAspect;\n"
                                      "in vec2 vUv;\n"
                                      "out vec4 oColor;\n"
                                      "void main() {\n"
                                      "    vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);\n"
                                      "    vec2 d = abs(p) - vec2(0.5 * uAspect - 0.5, 0.0);\n"
                                      "    float sd = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - 0.5;\n"
                                      "    float aa = fwidth(sd);\n"
                                      "    float alpha = min((1.0 - smoothstep(-aa, aa, sd)) * uGlow, 1.0);\n"
                                      "    oColor = vec4(vec3(alpha), alpha);\n"
                                      "}\n";
    // A quarter turn of a thick round-capped stroke, outside one corner of the picture. The quad is
    // centerd on that corner and the picture lies towards negative x and y, so the handle never
    // covers the game. Beyond the quarter the arc ends in its caps, which is what rounds the ends.
    static const char* CORNER_FRAGMENT =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform float uGlow;\n"
        "in vec2 vUv;\n"
        "out vec4 oColor;\n"
        "const float GAP = 0.125;\n"
        "const float RADIUS = 0.28;\n"
        "const float THICK = 0.05;\n"
        "void main() {\n"
        "    vec2 p = vUv - 0.5 - vec2(GAP - RADIUS);\n"
        "    float d = (p.x > 0.0 && p.y > 0.0)\n"
        "                  ? abs(length(p) - RADIUS)\n"
        "                  : min(length(p - vec2(RADIUS, 0.0)), length(p - vec2(0.0, RADIUS)));\n"
        "    d -= THICK;\n"
        "    float aa = fwidth(d);\n"
        "    float alpha = min((1.0 - smoothstep(-aa, aa, d)) * uGlow, 1.0);\n"
        "    oColor = vec4(vec3(alpha), alpha);\n"
        "}\n";

    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, VERTEX);
    mProgram = LinkProgram(vertex, FRAGMENT);
    mMenuProgram = LinkProgram(vertex, MENU_FRAGMENT);
    mCursorProgram = LinkProgram(vertex, CURSOR_FRAGMENT);
    mBarProgram = LinkProgram(vertex, BAR_FRAGMENT);
    mCornerProgram = LinkProgram(vertex, CORNER_FRAGMENT);
    const GLuint rayVertex = CompileShader(GL_VERTEX_SHADER, RAY_VERTEX);
    mRayProgram = LinkProgram(rayVertex, RAY_FRAGMENT);
    glDeleteShader(vertex);
    glDeleteShader(rayVertex);
    if (mProgram == 0 || mMenuProgram == 0 || mCursorProgram == 0 || mBarProgram == 0 || mCornerProgram == 0 ||
        mRayProgram == 0) {
        return false;
    }
    mMvpLoc = glGetUniformLocation(mProgram, "uMvp");
    mAspectLoc = glGetUniformLocation(mProgram, "uAspect");
    mRadiusLoc = glGetUniformLocation(mProgram, "uRadius");
    mFeatherLoc = glGetUniformLocation(mProgram, "uFeather");
    mShiftLoc = glGetUniformLocation(mProgram, "uShift");
    glUseProgram(mProgram);
    glUniform1i(glGetUniformLocation(mProgram, "uTex"), 0);
    glUseProgram(0);
    mMenuMvpLoc = glGetUniformLocation(mMenuProgram, "uMvp");
    mMenuGlowLoc = glGetUniformLocation(mMenuProgram, "uGlow");
    mCursorMvpLoc = glGetUniformLocation(mCursorProgram, "uMvp");
    mCursorDownLoc = glGetUniformLocation(mCursorProgram, "uDown");
    mCursorTintLoc = glGetUniformLocation(mCursorProgram, "uTint");
    mBarMvpLoc = glGetUniformLocation(mBarProgram, "uMvp");
    mBarGlowLoc = glGetUniformLocation(mBarProgram, "uGlow");
    mBarAspectLoc = glGetUniformLocation(mBarProgram, "uAspect");
    mCornerMvpLoc = glGetUniformLocation(mCornerProgram, "uMvp");
    mCornerGlowLoc = glGetUniformLocation(mCornerProgram, "uGlow");
    mRayMvpLoc = glGetUniformLocation(mRayProgram, "uMvp");
    mRayTintLoc = glGetUniformLocation(mRayProgram, "uTint");
    mRayWidthLoc = glGetUniformLocation(mRayProgram, "uWidth");
    mRayFadeLoc = glGetUniformLocation(mRayProgram, "uFade");
    mRayTaperLoc = glGetUniformLocation(mRayProgram, "uTaper");
    glGenVertexArrays(1, &mVao);
    return true;
}

// Column-major 4x4, as GL wants them.
static void MulMatrix(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] + a[8 + row] * b[col * 4 + 2] +
                                 a[12 + row] * b[col * 4 + 3];
        }
    }
}

static void RotationFromQuaternion(const XrQuaternionf& q, float r[9]) {
    r[0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    r[1] = 2.0f * (q.x * q.y + q.z * q.w);
    r[2] = 2.0f * (q.x * q.z - q.y * q.w);
    r[3] = 2.0f * (q.x * q.y - q.z * q.w);
    r[4] = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    r[5] = 2.0f * (q.y * q.z + q.x * q.w);
    r[6] = 2.0f * (q.x * q.z + q.y * q.w);
    r[7] = 2.0f * (q.y * q.z - q.x * q.w);
    r[8] = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
}

// This eye's own frustum and pose, which everything drawn in the room goes through.
static void EyeMatrix(const XrView& eye, float out[16]) {
    float r[9];
    RotationFromQuaternion(eye.pose.orientation, r);
    const XrVector3f& p = eye.pose.position;

    // The inverse of the eye pose, which for a rotation is its transpose.
    float view[16] = {};
    for (int axis = 0; axis < 3; axis++) {
        view[axis * 4] = r[axis];
        view[axis * 4 + 1] = r[3 + axis];
        view[axis * 4 + 2] = r[6 + axis];
        view[12 + axis] = -(r[axis * 3] * p.x + r[axis * 3 + 1] * p.y + r[axis * 3 + 2] * p.z);
    }
    view[15] = 1.0f;

    const float tl = tanf(eye.fov.angleLeft);
    const float tr = tanf(eye.fov.angleRight);
    const float td = tanf(eye.fov.angleDown);
    const float tu = tanf(eye.fov.angleUp);
    const float nearZ = 0.1f;
    const float farZ = 100.0f;
    float proj[16] = {};
    proj[0] = 2.0f / (tr - tl);
    proj[5] = 2.0f / (tu - td);
    proj[8] = (tr + tl) / (tr - tl);
    proj[9] = (tu + td) / (tu - td);
    proj[10] = -(farZ + nearZ) / (farZ - nearZ);
    proj[11] = -1.0f;
    proj[14] = -2.0f * farZ * nearZ / (farZ - nearZ);

    MulMatrix(proj, view, out);
}

// The window rectangle at the anchor pose, seen from the eye pose, through the eye's frustum.
static void PlacementMatrix(const XrView& eye, const XrPosef& anchor, float width, float height, float mvp[16]) {
    float r[9];

    // Columns 0 and 1 carry the rectangle's own size, so the unit quad comes out the window.
    RotationFromQuaternion(anchor.orientation, r);
    float model[16] = {};
    for (int axis = 0; axis < 3; axis++) {
        model[axis] = r[axis] * width;
        model[4 + axis] = r[3 + axis] * height;
        model[8 + axis] = r[6 + axis];
    }
    model[12] = anchor.position.x;
    model[13] = anchor.position.y;
    model[14] = anchor.position.z;
    model[15] = 1.0f;

    float eyeMatrix[16];
    EyeMatrix(eye, eyeMatrix);
    MulMatrix(eyeMatrix, model, mvp);
}

// A flat band from one point to another, turned to face the eye. The band's x runs along it from
// the hand and its y across, one meter wide, because the shader is what sets the width: a band held
// at one angle is wider at the end than at the start and no matrix can do that.
static void RayMatrix(const XrView& eye, const XrVector3f& from, const XrVector3f& to, float mvp[16]) {
    XrVector3f along = Subtract(to, from);
    const float span = Length(along);
    if (span <= 1e-5f) {
        return;
    }
    along = { along.x / span, along.y / span, along.z / span };

    const XrVector3f middle = { 0.5f * (from.x + to.x), 0.5f * (from.y + to.y), 0.5f * (from.z + to.z) };
    XrVector3f toEye = Subtract(middle, eye.pose.position);
    const float range = Length(toEye);
    if (range <= 1e-5f) {
        return;
    }
    toEye = { toEye.x / range, toEye.y / range, toEye.z / range };

    XrVector3f across = { along.y * toEye.z - along.z * toEye.y, along.z * toEye.x - along.x * toEye.z,
                          along.x * toEye.y - along.y * toEye.x };
    const float side = Length(across);
    if (side <= 1e-5f) {
        return;
    }
    across = { across.x / side, across.y / side, across.z / side };

    float model[16] = {};
    model[0] = along.x * span;
    model[1] = along.y * span;
    model[2] = along.z * span;
    model[4] = across.x;
    model[5] = across.y;
    model[6] = across.z;
    model[8] = -toEye.x;
    model[9] = -toEye.y;
    model[10] = -toEye.z;
    model[12] = middle.x;
    model[13] = middle.y;
    model[14] = middle.z;
    model[15] = 1.0f;

    float eyeMatrix[16];
    EyeMatrix(eye, eyeMatrix);
    MulMatrix(eyeMatrix, model, mvp);
}

// The game has just drawn this eye into the default framebuffer; keep a copy to place later.
void GfxWindowBackendOpenXR::PresentView(uint32_t view) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mGameFbo[view]);
    glBlitFramebuffer(0, 0, mGameWidth, mGameHeight, 0, 0, mTexWidth, mTexHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// The button hangs over the top edge of the window, on the same plane and in the same layer. It is
// outside the picture, so nothing the game draws has to make room for it. The cursor goes on the
// same plane, over the picture as readily as over the room, and over the button last of all.
void GfxWindowBackendOpenXR::DrawOverlays(uint32_t eye) {
    if (mMenuProgram == 0 || mCursorProgram == 0) {
        return;
    }
    // The game's renderer sets the blend function once, at start, and counts on it for the life of
    // the app. Put back what was found, or every alpha it draws from here on adds instead of blends.
    GLint blend[4] = {};
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(mVao);

    float mvp[16];
    const float side = MenuSide();
    PlacementMatrix(mViews[eye], PlanePose(0.0f, MenuRise()), side, side, mvp);
    glUseProgram(mMenuProgram);
    glUniformMatrix4fv(mMenuMvpLoc, 1, GL_FALSE, mvp);
    glUniform1f(mMenuGlowLoc, sMenuHeld ? 1.8f : (sMenuHover ? 1.3f : 1.0f));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // The bar is always there, dim, so the user knows the window can be moved. The corner handle
    // shows only for the corner the hand is at, which is how the picture keeps a clean frame.
    const bool moving = mGrab == Grab::Move;
    PlacementMatrix(mViews[eye], PlanePose(0.0f, BarDrop()), BarWidth(), BarHeight(), mvp);
    glUseProgram(mBarProgram);
    glUniformMatrix4fv(mBarMvpLoc, 1, GL_FALSE, mvp);
    glUniform1f(mBarAspectLoc, BarWidth() / BarHeight());
    glUniform1f(mBarGlowLoc, moving ? 0.95f : (mBarHover ? 0.7f : 0.35f));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (mCornerHover >= 0) {
        // One handle is drawn, and the quad is mirrored onto the corner the hand is at. A negative
        // side flips the shape with it.
        const float handle = CornerSide();
        const float signX = (mCornerHover & 1) != 0 ? 1.0f : -1.0f;
        const float signY = (mCornerHover & 2) != 0 ? 1.0f : -1.0f;
        PlacementMatrix(mViews[eye], PlanePose(signX * 0.5f * mWindowWidth, signY * 0.5f * mWindowHeight),
                        signX * handle, signY * handle, mvp);
        glUseProgram(mCornerProgram);
        glUniformMatrix4fv(mCornerMvpLoc, 1, GL_FALSE, mvp);
        glUniform1f(mCornerGlowLoc, mGrab == Grab::Resize ? 0.95f : 0.7f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // A dot for every hand that reaches the window, and a ray from the hand it came from. Both are
    // drawn even where only one of them answers, because two dots and no rays say nothing about
    // which hand is which.
    const float cursor = CursorSide();
    bool anyHand = false;
    for (int hand = 0; hand < 2; hand++) {
        if (!sHandValid[hand]) {
            continue;
        }
        anyHand = true;
        const XrPosef dot = PlanePose(sHandX[hand], sHandY[hand]);
        const float* tint = sHandDown[hand] ? POINTER_HELD : POINTER_IDLE;

        // The ray reaches as far as it is allowed or as far as it can while staying clear of the
        // ring, whichever is nearer. The gap is measured from the ring rather than from the middle
        // of it, or a ring wider than the gap would swallow the end of the ray. Below the length
        // its own ramps need, there is nothing left to draw.
        const XrVector3f& from = sHandFrom[hand];
        XrVector3f along = Subtract(dot.position, from);
        const float reach = Length(along);
        const float span = fminf(RAY_REACH, reach - cursor * CURSOR_RING - RAY_GAP);
        if (span > RAY_HIDDEN + 2.0f * RAY_RAMP) {
            const XrVector3f end = { from.x + along.x * span / reach, from.y + along.y * span / reach,
                                     from.z + along.z * span / reach };
            // One angle from end to end, taken from the width the near end should have.
            const float atHand = Length(Subtract(from, mViews[eye].pose.position));
            const float atEnd = Length(Subtract(end, mViews[eye].pose.position));
            const float wide = atHand > 1e-4f ? RAY_WIDTH * atEnd / atHand : RAY_WIDTH;

            RayMatrix(mViews[eye], from, end, mvp);
            glUseProgram(mRayProgram);
            glUniformMatrix4fv(mRayMvpLoc, 1, GL_FALSE, mvp);
            glUniform2f(mRayWidthLoc, RAY_WIDTH, wide);
            glUniform4f(mRayFadeLoc, RAY_HIDDEN, RAY_HIDDEN + RAY_RAMP, span - RAY_RAMP, span);
            glUniform2f(mRayTaperLoc, span - RAY_RAMP - RAY_TAPER, RAY_TAPER_TO);
            glUniform3fv(mRayTintLoc, 1, tint);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        PlacementMatrix(mViews[eye], dot, cursor, cursor, mvp);
        glUseProgram(mCursorProgram);
        glUniformMatrix4fv(mCursorMvpLoc, 1, GL_FALSE, mvp);
        glUniform1f(mCursorDownLoc, sHandDown[hand] ? 1.0f : 0.0f);
        glUniform3fv(mCursorTintLoc, 1, tint);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Nothing else puts a pointer on the window but a hand, except the one driven from the host.
    if (!anyHand && sCursorValid) {
        PlacementMatrix(mViews[eye], PlanePose(sCursorX, sCursorY), cursor, cursor, mvp);
        glUseProgram(mCursorProgram);
        glUniformMatrix4fv(mCursorMvpLoc, 1, GL_FALSE, mvp);
        const bool held = sPointerDown || sMenuHeld;
        glUniform1f(mCursorDownLoc, held ? 1.0f : 0.0f);
        glUniform3fv(mCursorTintLoc, 1, held ? POINTER_HELD : POINTER_IDLE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glBlendFuncSeparate(blend[0], blend[1], blend[2], blend[3]);
}

// Draws the captured game image onto the window rectangle inside this eye's full view. Alpha
// stays zero everywhere else, so the room shows through around the window.
void GfxWindowBackendOpenXR::DrawEye(uint32_t eye, uint32_t sourceView) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (Failed(mInstance, xrAcquireSwapchainImage(mSwapchain[eye], &acquireInfo, &index), "xrAcquireSwapchainImage")) {
        return;
    }

    XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (!Failed(mInstance, xrWaitSwapchainImage(mSwapchain[eye], &waitInfo), "xrWaitSwapchainImage")) {
        if (mSrgbWriteControl) {
            glDisable(GL_FRAMEBUFFER_SRGB_EXT);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, mImageFbos[eye][index]);
        glViewport(0, 0, mSwapchainWidth, mSwapchainHeight);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        float mvp[16];
        PlacementMatrix(mViews[eye], mAnchorPose, mWindowWidth, mWindowHeight, mvp);
        glUseProgram(mProgram);
        glUniformMatrix4fv(mMvpLoc, 1, GL_FALSE, mvp);
        glUniform1f(mAspectLoc, mWindowHeight > 0.0f ? mWindowWidth / mWindowHeight : 1.0f);
        glUniform1f(mRadiusLoc, CORNER_SIDE * CORNER_ARC_CENTER);
        glUniform1f(mFeatherLoc, WINDOW_FEATHER * sEdgeSoftness);
        // The left eye gives up the left edge and the right eye the right one. One image for both
        // eyes has no parallax to correct, so it gives up nothing.
        glUniform1f(mShiftLoc, eye == 0 ? EdgeFloat() : -EdgeFloat());
        glBindVertexArray(mVao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mGameTex[sourceView]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);

        DrawOverlays(eye);

#ifdef ENABLE_DEBUG_TOOLS
        if (DebugCapture::Pending()) {
            DebugCapture::WriteBoundFramebuffer(eye == 0 ? "left" : "right", mSwapchainWidth, mSwapchainHeight);
            if (eye + 1 >= VIEW_COUNT) {
                DebugCapture::Finish();
            }
        }
#endif
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    Failed(mInstance, xrReleaseSwapchainImage(mSwapchain[eye], &releaseInfo), "xrReleaseSwapchainImage");
}

void GfxWindowBackendOpenXR::EndRenderFrame() {
    XrCompositionLayerProjectionView projectionViews[VIEW_COUNT];
    XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerPassthroughFB passthrough{ XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
    const XrCompositionLayerBaseHeader* layers[2] = {};
    uint32_t layerCount = 0;

    // The room is a layer of its own where the environment cannot be alpha blended, and it has to
    // be under the frame for the game to draw over it.
    if (mPassthroughLayer != XR_NULL_HANDLE) {
        passthrough.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        passthrough.space = XR_NULL_HANDLE;
        passthrough.layerHandle = mPassthroughLayer;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&passthrough;
    }

    if (mShouldRender && mViewsValid && mAnchorValid) {
        for (uint32_t eye = 0; eye < VIEW_COUNT; eye++) {
            DrawEye(eye, mViewCount == VIEW_COUNT ? eye : 0);
            projectionViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            projectionViews[eye].pose = mViews[eye].pose;
            projectionViews[eye].fov = mViews[eye].fov;
            projectionViews[eye].subImage.swapchain = mSwapchain[eye];
            projectionViews[eye].subImage.imageRect = { { 0, 0 },
                                                        { (int32_t)mSwapchainWidth, (int32_t)mSwapchainHeight } };
            projectionViews[eye].subImage.imageArrayIndex = 0;
        }
        // Everything drawn over the picture blends into the same image, and a blend that keeps the
        // alpha right can only work on color already multiplied by it. The layer reads it the same way.
        projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        projection.space = mSpace;
        projection.viewCount = VIEW_COUNT;
        projection.views = projectionViews;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&projection;
    }

    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = mDisplayTime;
    endInfo.environmentBlendMode = mBlendMode;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    Failed(mInstance, xrEndFrame(mSession, &endInfo), "xrEndFrame");

    mFrameOpen = false;
    sViewGeometryValid = false;
}

void GfxWindowBackendOpenXR::SwapBuffersBegin() {
    if (!mActive) {
        GfxWindowBackendSDL2::SwapBuffersBegin();
        return;
    }
    if (!mFrameOpen) {
        // A caller that draws the gui by itself, as the extractor's progress screen does, asks for
        // no views at all. Open a frame for it and let both eyes read the one image.
        if (!OpenFrame()) {
            return;
        }
        mViewCount = 1;
    }

#ifdef ENABLE_DEBUG_TOOLS
    // The picture as the game drew it, in the coordinates the debug pointer is aimed in.
    if (mCurrentView == 0 && DebugCapture::Pending()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        DebugCapture::WriteBoundFramebuffer("panel", mGameWidth, mGameHeight);
    }
#endif

    if (mShouldRender) {
        PresentView(mCurrentView);
    }
    if (mCurrentView + 1 >= mViewCount) {
        EndRenderFrame();
    }
}

void GfxWindowBackendOpenXR::Teardown() {
    for (uint32_t view = 0; view < VIEW_COUNT; view++) {
        if (!mImageFbos[view].empty()) {
            glDeleteFramebuffers((GLsizei)mImageFbos[view].size(), mImageFbos[view].data());
            mImageFbos[view].clear();
        }
        if (mGameFbo[view] != 0) {
            glDeleteFramebuffers(1, &mGameFbo[view]);
            mGameFbo[view] = 0;
        }
        if (mGameTex[view] != 0) {
            glDeleteTextures(1, &mGameTex[view]);
            mGameTex[view] = 0;
        }
        if (mSwapchain[view] != XR_NULL_HANDLE) {
            xrDestroySwapchain(mSwapchain[view]);
            mSwapchain[view] = XR_NULL_HANDLE;
        }
    }
    if (mProgram != 0) {
        glDeleteProgram(mProgram);
        mProgram = 0;
    }
    if (mMenuProgram != 0) {
        glDeleteProgram(mMenuProgram);
        mMenuProgram = 0;
    }
    if (mCursorProgram != 0) {
        glDeleteProgram(mCursorProgram);
        mCursorProgram = 0;
    }
    if (mBarProgram != 0) {
        glDeleteProgram(mBarProgram);
        mBarProgram = 0;
    }
    if (mCornerProgram != 0) {
        glDeleteProgram(mCornerProgram);
        mCornerProgram = 0;
    }
    if (mRayProgram != 0) {
        glDeleteProgram(mRayProgram);
        mRayProgram = 0;
    }
    if (mVao != 0) {
        glDeleteVertexArrays(1, &mVao);
        mVao = 0;
    }
    if (mPassthroughLayer != XR_NULL_HANDLE && mDestroyPassthroughLayer != nullptr) {
        mDestroyPassthroughLayer(mPassthroughLayer);
        mPassthroughLayer = XR_NULL_HANDLE;
    }
    if (mPassthrough != XR_NULL_HANDLE && mDestroyPassthrough != nullptr) {
        mDestroyPassthrough(mPassthrough);
        mPassthrough = XR_NULL_HANDLE;
    }
    if (mAnchorSpace != XR_NULL_HANDLE) {
        xrDestroySpace(mAnchorSpace);
        mAnchorSpace = XR_NULL_HANDLE;
    }
    mCreateAnchorSpace = nullptr;
    if (mLocalSpace != XR_NULL_HANDLE) {
        xrDestroySpace(mLocalSpace);
        mLocalSpace = XR_NULL_HANDLE;
    }
    if (mSpace != XR_NULL_HANDLE) {
        xrDestroySpace(mSpace);
        mSpace = XR_NULL_HANDLE;
    }
    if (mSession != XR_NULL_HANDLE) {
        xrDestroySession(mSession);
        mSession = XR_NULL_HANDLE;
    }
    if (mInstance != XR_NULL_HANDLE) {
        xrDestroyInstance(mInstance);
        mInstance = XR_NULL_HANDLE;
    }
    if (mActivity != nullptr) {
        JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
        if (env != nullptr) {
            env->DeleteGlobalRef(mActivity);
        }
        mActivity = nullptr;
    }
    mRequestRefreshRate = nullptr;
    mRefreshRates.clear();
    mRefreshRate = 0.0f;
    mActive = false;
    mRunning = false;
    mFrameOpen = false;
    sViewGeometryValid = false;
}

void GfxWindowBackendOpenXR::Destroy() {
    Teardown();
    GfxWindowBackendSDL2::Destroy();
}

} // namespace Fast

#endif
