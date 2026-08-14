#ifdef ENABLE_OPENXR

#include "fast/backends/gfx_openxr.h"

#include <cmath>
#include <cstring>
#include <string>

#include <android/log.h>
#include <GLES3/gl3.h>

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#include "fast/backends/gfx_debug_capture.h"

namespace Fast {

// The window hangs this far in front of where the user faced at start. It is as wide as the game's
// own field of view needs it to be, so the framing does not change; the size below only holds
// until the first frame says otherwise. Part 2.3 turns all of this into settings.
static constexpr float WINDOW_DISTANCE_METRES = 2.0f;
static constexpr float WINDOW_WIDTH_METRES = 1.6f;

// Game units from the viewpoint to the glass. Everything nearer than this stands in front of the
// window, everything further recedes behind it, and the number therefore sets how large the world
// is: Banjo sits about this far from the camera.
static constexpr float WINDOW_DEPTH_UNITS = 700.0f;

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

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (Failed(mInstance, xrCreateReferenceSpace(mSession, &spaceInfo, &mSpace), "xrCreateReferenceSpace")) {
        return false;
    }

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
    // The game draws display-referred colour. A linear swapchain makes the runtime encode it to
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
    mSwapchainWidth = width;
    mSwapchainHeight = height;

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

    mWindowWidth = WINDOW_WIDTH_METRES;
    mWindowHeight = WINDOW_WIDTH_METRES * (float)mSwapchainHeight / (float)mSwapchainWidth;

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
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "asked the display for %.0f Hz", rate);
    return true;
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

static XrVector3f RotateByQuaternion(const XrQuaternionf& q, const XrVector3f& v) {
    const float x = q.y * v.z - q.z * v.y + q.w * v.x;
    const float y = q.z * v.x - q.x * v.z + q.w * v.y;
    const float z = q.x * v.y - q.y * v.x + q.w * v.z;
    return { v.x + 2.0f * (q.y * z - q.z * y), v.y + 2.0f * (q.z * x - q.x * z), v.z + 2.0f * (q.x * y - q.y * x) };
}

static bool sPointerValid = false;
static bool sPointerDown = false;
static float sPointerU = 0.0f;
static float sPointerV = 0.0f;

bool GetXrPointer(float* u, float* v, bool* down) {
    if (!sPointerValid) {
        return false;
    }
    *u = sPointerU;
    *v = sPointerV;
    *down = sPointerDown;
    return true;
}

static bool sViewGeometryValid = false;
static XrViewGeometry sViewGeometry = {};
static float sViewTanHalfWidth = 0.0f;
static float sViewTanHalfHeight = 0.0f;
static float sWindowAngularWidth = 0.0f;

bool GetXrViewGeometry(XrViewGeometry* geometry) {
    if (!sViewGeometryValid) {
        return false;
    }
    *geometry = sViewGeometry;
    return true;
}

void SetXrViewTangents(float tanHalfWidth, float tanHalfHeight) {
    sViewTanHalfWidth = tanHalfWidth;
    sViewTanHalfHeight = tanHalfHeight;
}

float GetXrWindowAngularWidth() {
    return sWindowAngularWidth;
}

void GfxWindowBackendOpenXR::PumpPointer(XrTime displayTime) {
    if (mActionSet == XR_NULL_HANDLE || mState != XR_SESSION_STATE_FOCUSED) {
        return;
    }

    XrActiveActionSet active{ mActionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    if (Failed(mInstance, xrSyncActions(mSession, &syncInfo), "xrSyncActions")) {
        return;
    }

    bool hit = false;
    bool down = false;
    float u = 0.0f;
    float v = 0.0f;

    for (int hand = 0; hand < 2; hand++) {
        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace(mAimSpace[hand], mSpace, displayTime, &location)) ||
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 ||
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
            continue;
        }

        const XrVector3f origin = location.pose.position;
        const XrVector3f forward = RotateByQuaternion(location.pose.orientation, { 0.0f, 0.0f, -1.0f });
        if (forward.z >= -1e-4f) {
            continue;
        }
        const float t = (-WINDOW_DISTANCE_METRES - origin.z) / forward.z;
        if (t <= 0.0f) {
            continue;
        }

        const float hitU = (origin.x + t * forward.x) / mWindowWidth + 0.5f;
        const float hitV = 0.5f - (origin.y + t * forward.y) / mWindowHeight;
        if (hitU < 0.0f || hitU > 1.0f || hitV < 0.0f || hitV > 1.0f) {
            continue;
        }

        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.action = mSelectAction;
        getInfo.subactionPath = mHandPath[hand];
        XrActionStateBoolean select{ XR_TYPE_ACTION_STATE_BOOLEAN };
        const bool pinching = XR_SUCCEEDED(xrGetActionStateBoolean(mSession, &getInfo, &select)) && select.isActive &&
                              select.currentState == XR_TRUE;

        // A pinching hand wins; otherwise the first hand on the quad drives the cursor.
        if (!hit || pinching) {
            hit = true;
            down = pinching;
            u = hitU;
            v = hitV;
        }
        if (pinching) {
            break;
        }
    }

    const bool wasDown = sPointerDown;
    if (hit) {
        sPointerValid = true;
        sPointerU = u;
        sPointerV = v;
    } else if (!wasDown) {
        sPointerValid = false;
    }

    if (!hit && !wasDown) {
        return;
    }

    // The pad reads SDL's touch device list, which SDL_PushEvent cannot reach, so the pinch goes
    // in there directly. ImGui reads mouse events, which is what SDL synthesises from a touch on a
    // phone, so the menu is driven the same way here.
    const int x = (int)(sPointerU * (float)mSwapchainWidth);
    const int y = (int)(sPointerV * (float)mSwapchainHeight);

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
        } else if (event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            // The runtime can refuse or later drop the rate the game asked for, and the sub-frame
            // count follows whatever it reports here.
            mRefreshRate = ((const XrEventDataDisplayRefreshRateChangedFB*)&event)->toDisplayRefreshRate;
            __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "display now runs at %.0f Hz", mRefreshRate);
        }
    }
}

void GfxWindowBackendOpenXR::HandleStateChange(const XrEventDataSessionStateChanged& changed) {
    mState = changed.state;
    SPDLOG_INFO("OpenXR: session state {}", (int)mState);

    if (mState == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        mRunning = !Failed(mInstance, xrBeginSession(mSession, &beginInfo), "xrBeginSession");
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

void GfxWindowBackendOpenXR::SizeWindow() {
    if (mWindowSized || sViewTanHalfWidth <= 0.0f || sViewTanHalfHeight <= 0.0f) {
        return;
    }
    mWindowWidth = 2.0f * WINDOW_DISTANCE_METRES * sViewTanHalfWidth;
    mWindowHeight = 2.0f * WINDOW_DISTANCE_METRES * sViewTanHalfHeight;
    mWindowSized = true;
    sWindowAngularWidth = 2.0f * atanf(0.5f * mWindowWidth / WINDOW_DISTANCE_METRES);
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "window %.2f x %.2f m at %.1f m, %.1f degrees wide",
                        mWindowWidth, mWindowHeight, WINDOW_DISTANCE_METRES,
                        sWindowAngularWidth * 180.0f / (float)M_PI);
}

bool GfxWindowBackendOpenXR::OpenFrame() {
    sViewGeometryValid = false;
    if (!mActive) {
        return false;
    }

    PollEvents();
    if (!mRunning) {
        return false;
    }

    SizeWindow();

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
    PumpPointer(mDisplayTime);

    // Without a head pose there is no off-axis frustum to build, so the frame falls back to one
    // image that both eyes read, which is what 2.1 presented.
    mViewCount = mViewsValid ? VIEW_COUNT : 1;
    mCurrentView = 0;
    return true;
}

uint32_t GfxWindowBackendOpenXR::BeginRenderFrame() {
    return OpenFrame() ? mViewCount : 1;
}

void GfxWindowBackendOpenXR::BeginRenderView(uint32_t view) {
    mCurrentView = view;
    sViewGeometryValid = false;
    if (!mFrameOpen || !mViewsValid || view >= VIEW_COUNT) {
        return;
    }

    // The window plane is a copy of the game's own screen, so the head offset converts with the
    // one scale that puts the glass WINDOW_DEPTH_UNITS from the viewpoint. LOCAL space starts at
    // the head, and the window hangs straight ahead of it, so the eye pose is already the offset.
    const float unitsPerMetre = WINDOW_DEPTH_UNITS / WINDOW_DISTANCE_METRES;
    const XrVector3f& position = mViews[view].pose.position;
    sViewGeometry.eyeOffset[0] = position.x * unitsPerMetre;
    sViewGeometry.eyeOffset[1] = position.y * unitsPerMetre;
    sViewGeometry.eyeOffset[2] = position.z * unitsPerMetre;
    sViewGeometry.windowDistance = WINDOW_DEPTH_UNITS;
    sViewGeometryValid = true;
}

void GfxWindowBackendOpenXR::PresentView(uint32_t view) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (Failed(mInstance, xrAcquireSwapchainImage(mSwapchain[view], &acquireInfo, &index), "xrAcquireSwapchainImage")) {
        return;
    }

    XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (!Failed(mInstance, xrWaitSwapchainImage(mSwapchain[view], &waitInfo), "xrWaitSwapchainImage")) {
        // The game has just drawn this eye into the default framebuffer. A GLES swapchain image is
        // an ordinary GL texture, so it keeps the bottom-up orientation and the blit is 1:1.
        if (mSrgbWriteControl) {
            glDisable(GL_FRAMEBUFFER_SRGB_EXT);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mImageFbos[view][index]);
        glBlitFramebuffer(0, 0, mSwapchainWidth, mSwapchainHeight, 0, 0, mSwapchainWidth, mSwapchainHeight,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);

        if (DebugCapture::Pending()) {
            glBindFramebuffer(GL_FRAMEBUFFER, mImageFbos[view][index]);
            DebugCapture::WriteBoundFramebuffer(view == 0 ? "left" : "right", mSwapchainWidth, mSwapchainHeight);
            if (view + 1 >= mViewCount) {
                DebugCapture::Finish();
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    Failed(mInstance, xrReleaseSwapchainImage(mSwapchain[view], &releaseInfo), "xrReleaseSwapchainImage");
}

void GfxWindowBackendOpenXR::EndRenderFrame() {
    XrCompositionLayerQuad quads[VIEW_COUNT] = {};
    const XrCompositionLayerBaseHeader* layers[VIEW_COUNT] = {};
    uint32_t layerCount = 0;

    if (mShouldRender) {
        static constexpr XrEyeVisibility VISIBILITY[VIEW_COUNT] = { XR_EYE_VISIBILITY_LEFT, XR_EYE_VISIBILITY_RIGHT };
        for (uint32_t view = 0; view < mViewCount; view++) {
            quads[view] = { XR_TYPE_COMPOSITION_LAYER_QUAD };
            // No source-alpha bit: the game does not keep a meaningful alpha channel, and honouring
            // it under alpha blend makes parts of the picture see-through.
            quads[view].layerFlags = 0;
            quads[view].space = mSpace;
            quads[view].eyeVisibility = mViewCount == VIEW_COUNT ? VISIBILITY[view] : XR_EYE_VISIBILITY_BOTH;
            quads[view].subImage.swapchain = mSwapchain[view];
            quads[view].subImage.imageRect = { { 0, 0 }, { (int32_t)mSwapchainWidth, (int32_t)mSwapchainHeight } };
            quads[view].subImage.imageArrayIndex = 0;
            quads[view].pose.orientation.w = 1.0f;
            quads[view].pose.position.z = -WINDOW_DISTANCE_METRES;
            quads[view].size.width = mWindowWidth;
            quads[view].size.height = mWindowHeight;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&quads[view];
        }
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
        if (mSwapchain[view] != XR_NULL_HANDLE) {
            xrDestroySwapchain(mSwapchain[view]);
            mSwapchain[view] = XR_NULL_HANDLE;
        }
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
