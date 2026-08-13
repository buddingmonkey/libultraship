#ifdef ENABLE_OPENXR

#include "fast/backends/gfx_openxr.h"

#include <cstring>
#include <string>

#include <android/log.h>
#include <GLES3/gl3.h>
#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#include "fast/backends/gfx_debug_capture.h"

namespace Fast {

// The quad hangs this far in front of where the user faced at start, and is this wide. Part 2.3
// turns both into settings.
static constexpr float QUAD_DISTANCE_METRES = 2.0f;
static constexpr float QUAD_WIDTH_METRES = 1.6f;

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

    const char* enabled[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };

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
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.enabledExtensionNames = enabled;
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
    // The game draws display-referred colour into the default framebuffer. An sRGB swapchain makes
    // the blit encode it a second time, which washes the picture out and turns red pink.
    int64_t format = formats.empty() ? 0 : formats[0];
    for (int64_t candidate : formats) {
        if (candidate == GL_RGBA8 || candidate == GL_RGB8) {
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

    XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = format;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = mSwapchainWidth;
    swapchainInfo.height = mSwapchainHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    if (Failed(mInstance, xrCreateSwapchain(mSession, &swapchainInfo, &mSwapchain), "xrCreateSwapchain")) {
        return false;
    }

    uint32_t imageCount = 0;
    if (Failed(mInstance, xrEnumerateSwapchainImages(mSwapchain, 0, &imageCount, nullptr),
               "xrEnumerateSwapchainImages")) {
        return false;
    }
    mImages.assign(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
    if (Failed(mInstance,
               xrEnumerateSwapchainImages(mSwapchain, imageCount, &imageCount,
                                          (XrSwapchainImageBaseHeader*)mImages.data()),
               "xrEnumerateSwapchainImages")) {
        return false;
    }

    mImageFbos.resize(imageCount);
    glGenFramebuffers(imageCount, mImageFbos.data());
    for (uint32_t i = 0; i < imageCount; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, mImageFbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mImages[i].image, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR", "session up: %u images %ux%u, format 0x%04x, blend mode %d",
                        imageCount, mSwapchainWidth, mSwapchainHeight, (unsigned)format, (int)mBlendMode);
    return true;
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

void GfxWindowBackendOpenXR::PresentQuad() {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (Failed(mInstance, xrAcquireSwapchainImage(mSwapchain, &acquireInfo, &index), "xrAcquireSwapchainImage")) {
        return;
    }

    XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (!Failed(mInstance, xrWaitSwapchainImage(mSwapchain, &waitInfo), "xrWaitSwapchainImage")) {
        // The game has just drawn its frame into the default framebuffer. A GLES swapchain image is
        // an ordinary GL texture, so it keeps the bottom-up orientation and the blit is 1:1.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mImageFbos[index]);
        glBlitFramebuffer(0, 0, mSwapchainWidth, mSwapchainHeight, 0, 0, mSwapchainWidth, mSwapchainHeight,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);

        if (DebugCapture::Pending()) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            DebugCapture::WriteBoundFramebuffer("source", mSwapchainWidth, mSwapchainHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, mImageFbos[index]);
            DebugCapture::WriteBoundFramebuffer("quad", mSwapchainWidth, mSwapchainHeight);
            DebugCapture::Finish();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    Failed(mInstance, xrReleaseSwapchainImage(mSwapchain, &releaseInfo), "xrReleaseSwapchainImage");
}

void GfxWindowBackendOpenXR::SwapBuffersBegin() {
    if (!mActive) {
        GfxWindowBackendSDL2::SwapBuffersBegin();
        return;
    }

    PollEvents();
    if (!mRunning) {
        return;
    }

    XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    if (Failed(mInstance, xrWaitFrame(mSession, &waitInfo, &frameState), "xrWaitFrame")) {
        return;
    }

    XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
    if (Failed(mInstance, xrBeginFrame(mSession, &beginInfo), "xrBeginFrame")) {
        return;
    }

    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[] = { (const XrCompositionLayerBaseHeader*)&quad };
    uint32_t layerCount = 0;

    if (frameState.shouldRender == XR_TRUE) {
        PresentQuad();

        // No source-alpha bit: the game does not keep a meaningful alpha channel, and honouring it
        // under alpha blend makes parts of the picture see-through.
        quad.layerFlags = 0;
        quad.space = mSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = mSwapchain;
        quad.subImage.imageRect = { { 0, 0 }, { (int32_t)mSwapchainWidth, (int32_t)mSwapchainHeight } };
        quad.subImage.imageArrayIndex = 0;
        quad.pose.orientation.w = 1.0f;
        quad.pose.position.z = -QUAD_DISTANCE_METRES;
        quad.size.width = QUAD_WIDTH_METRES;
        quad.size.height = QUAD_WIDTH_METRES * (float)mSwapchainHeight / (float)mSwapchainWidth;
        layerCount = 1;
    }

    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = mBlendMode;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    Failed(mInstance, xrEndFrame(mSession, &endInfo), "xrEndFrame");
}

void GfxWindowBackendOpenXR::Teardown() {
    if (!mImageFbos.empty()) {
        glDeleteFramebuffers((GLsizei)mImageFbos.size(), mImageFbos.data());
        mImageFbos.clear();
    }
    if (mSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(mSwapchain);
        mSwapchain = XR_NULL_HANDLE;
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
    mActive = false;
    mRunning = false;
}

void GfxWindowBackendOpenXR::Destroy() {
    Teardown();
    GfxWindowBackendSDL2::Destroy();
}

} // namespace Fast

#endif
