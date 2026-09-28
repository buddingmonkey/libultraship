#pragma once

#ifdef ENABLE_OPENXR

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>
#include <EGL/egl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace Fast {

class XrVirtualKeyboard {
  public:
    static bool Wanted(const std::vector<XrExtensionProperties>& extensions);
    static void AddExtensions(std::vector<const char*>* enabled);

    bool Start(XrInstance instance, XrSystemId system, XrSession session, XrSpace space);
    void Stop();
    bool Available() const {
        return mKeyboard != XR_NULL_HANDLE && mModelLoaded;
    }
    ~XrVirtualKeyboard();

    void Poll();
    void SetVisible(bool visible, const char* textContext);
    bool Visible() const {
        return mShown;
    }
    bool HandleEvent(const XrEventDataBuffer& event);

    float Aspect() const;
    void Place(const XrPosef& pose, float width);
    void Update(XrTime time);
    bool RayHit(const XrPosef& aim, XrVector3f* point) const;
    bool SurfacePose(const XrPosef& aim, XrPosef* pose) const;
    void SendRay(int hand, bool handTracked, const XrPosef& aim, bool pressed);
    void Draw(const float viewProjection[16]);
    bool KeyboardPoint(float x, float y, XrVector3f* point, XrVector3f* normal) const;

    std::string TakeText();
    int TakeBackspaces();
    bool TakeEnter();
    bool TakeClosed();

  private:
    struct Accessor;
    struct Primitive {
        std::vector<float> position;
        std::vector<float> uv;
        std::vector<std::vector<float>> targetPosition;
        std::vector<std::vector<float>> targetUv;
        std::vector<uint32_t> indices;
        std::vector<float> morphedPosition;
        std::vector<float> morphedUv;
        int material = -1;
    };
    struct Mesh {
        std::vector<Primitive> primitives;
        std::vector<float> weights;
    };
    struct Node {
        std::string name;
        int parent = -1;
        std::vector<int> children;
        int mesh = -1;
        float translation[3] = { 0.0f, 0.0f, 0.0f };
        float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float scale[3] = { 1.0f, 1.0f, 1.0f };
        bool hasMatrix = false;
        float matrix[16] = {};
        std::vector<float> weights;
        float global[16] = {};
        bool transformDirty = true;
        bool geometryDirty = true;
        bool vertexDirty = true;
        std::vector<int> slots;
    };
    struct Material {
        float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        int texture = -1;
    };
    struct Texture {
        uint32_t gl = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    struct Image {
        bool live = false;
        uint64_t id = 0;
        uint32_t width = 1;
        uint32_t height = 1;
        std::vector<uint8_t> pixels;
    };
    struct Slot {
        int node = -1;
        int mesh = -1;
        int primitive = -1;
        uint32_t offset = 0;
    };
    struct Sampler {
        std::vector<float> times;
        std::vector<float> values;
        int components = 0;
        bool step = false;
    };
    enum class Path { Translation, Rotation, Scale, Weights };
    struct Channel {
        int sampler = -1;
        int node = -1;
        Path path = Path::Translation;
        int additiveWeightIndex = -1;
    };
    struct Animation {
        std::vector<Sampler> samplers;
        std::vector<Channel> channels;
    };
    struct Batch {
        int texture = -1;
        float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        uint32_t first = 0;
        uint32_t count = 0;
    };

    enum class Load { Idle, Running, Retry, Done, Failed };

    Load LoadModel();
    bool ParseModel(const std::vector<uint8_t>& glb);
    void FinishModel();
    bool StartRenderer();
    void BuildLayout();
    void RefreshVertices();
    void UpdateTextures();
    void UpdateAnimations();
    void ApplyAnimation(int index, float fraction);
    void UpdateTransforms();
    void ModelMatrix(float out[16]) const;
    bool Locate(XrTime time);
    void Suggest();

    XrInstance mInstance = XR_NULL_HANDLE;
    XrSession mSession = XR_NULL_HANDLE;
    XrSpace mSpace = XR_NULL_HANDLE;
    XrVirtualKeyboardMETA mKeyboard = XR_NULL_HANDLE;
    XrSpace mKeyboardSpace = XR_NULL_HANDLE;

    PFN_xrCreateVirtualKeyboardMETA mCreate = nullptr;
    PFN_xrDestroyVirtualKeyboardMETA mDestroy = nullptr;
    PFN_xrCreateVirtualKeyboardSpaceMETA mCreateSpace = nullptr;
    PFN_xrSuggestVirtualKeyboardLocationMETA mSuggestLocation = nullptr;
    PFN_xrGetVirtualKeyboardScaleMETA mGetScale = nullptr;
    PFN_xrSetVirtualKeyboardModelVisibilityMETA mSetVisibility = nullptr;
    PFN_xrGetVirtualKeyboardModelAnimationStatesMETA mGetAnimationStates = nullptr;
    PFN_xrGetVirtualKeyboardDirtyTexturesMETA mGetDirtyTextures = nullptr;
    PFN_xrGetVirtualKeyboardTextureDataMETA mGetTextureData = nullptr;
    PFN_xrSendVirtualKeyboardInputMETA mSendInput = nullptr;
    PFN_xrChangeVirtualKeyboardTextContextMETA mChangeTextContext = nullptr;
    PFN_xrEnumerateRenderModelPathsFB mEnumerateModelPaths = nullptr;
    PFN_xrGetRenderModelPropertiesFB mGetModelProperties = nullptr;
    PFN_xrLoadRenderModelFB mLoadModel = nullptr;

    std::vector<Node> mNodes;
    std::vector<Mesh> mMeshes;
    std::vector<Material> mMaterials;
    std::vector<int> mTextureOfGltfTexture;
    std::vector<Texture> mTextures;
    std::vector<Image> mImages;
    std::string mModelName;
    std::map<uint64_t, int> mDynamicTextures;
    std::vector<Animation> mAnimations;
    float mAnimationStart = 0.0f;
    float mAnimationEnd = 0.0f;
    int mCollisionNode = -1;
    float mCollisionMin[3] = {};
    float mCollisionMax[3] = {};
    float mVisualMin[3] = {};
    float mVisualMax[3] = {};
    bool mModelLoaded = false;
    int mLoadTries = 0;
    int mLoadWait = 0;
    std::thread mLoader;
    std::atomic<Load> mLoad{ Load::Idle };
    bool mTransformsDirty = true;
    bool mVerticesDirty = true;

    uint32_t mProgram = 0;
    int32_t mMvpLoc = -1;
    int32_t mBaseColorLoc = -1;
    uint32_t mVao = 0;
    uint32_t mWhite = 0;
    uint32_t mVbo = 0;
    uint32_t mIbo = 0;
    std::vector<float> mVertices;
    std::vector<uint32_t> mIndices;
    std::vector<Batch> mBatches;
    std::vector<Slot> mSlots;
    std::vector<uint8_t> mTextureBuffer;
    std::vector<XrVirtualKeyboardAnimationStateMETA> mAnimationStates;

    XrPosef mWantedPose = { { 0.0f, 0.0f, 0.0f, 1.0f }, {} };
    float mWantedScale = 1.0f;
    bool mSuggestPending = false;
    XrPosef mPose = { { 0.0f, 0.0f, 0.0f, 1.0f }, {} };
    float mScale = 1.0f;
    bool mPoseValid = false;
    bool mWantVisible = false;
    bool mShown = false;

    std::string mText;
    int mBackspaces = 0;
    bool mEnter = false;
    bool mClosed = false;
};

} // namespace Fast

#endif
