#ifdef ENABLE_OPENXR

#include "fast/backends/gfx_openxr_keyboard.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace Fast {

static constexpr const char* LOG_TAG = "ShipXR";
static constexpr const char* MODEL_PATH = "/model_meta/keyboard/virtual";
static constexpr const char* TEXTURE_URI = "metaVirtualKeyboard://texture/";
static constexpr float HIT_MARGIN = 0.02f;
static constexpr int LOAD_TRIES = 40;
static constexpr int LOAD_INTERVAL = 30;

static void Multiply(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] + a[8 + row] * b[col * 4 + 2] +
                               a[12 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

static void FromTrs(const float t[3], const float q[4], const float s[3], float m[16]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = (1.0f - 2.0f * (y * y + z * z)) * s[0];
    m[1] = (2.0f * (x * y + z * w)) * s[0];
    m[2] = (2.0f * (x * z - y * w)) * s[0];
    m[3] = 0.0f;
    m[4] = (2.0f * (x * y - z * w)) * s[1];
    m[5] = (1.0f - 2.0f * (x * x + z * z)) * s[1];
    m[6] = (2.0f * (y * z + x * w)) * s[1];
    m[7] = 0.0f;
    m[8] = (2.0f * (x * z + y * w)) * s[2];
    m[9] = (2.0f * (y * z - x * w)) * s[2];
    m[10] = (1.0f - 2.0f * (x * x + y * y)) * s[2];
    m[11] = 0.0f;
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1.0f;
}

static void TransformPoint(const float m[16], const float p[3], float out[3]) {
    float r[3];
    for (int i = 0; i < 3; i++) {
        r[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
    }
    memcpy(out, r, sizeof(r));
}

static void TransformVector(const float m[16], const float v[3], float out[3]) {
    float r[3];
    for (int i = 0; i < 3; i++) {
        r[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2];
    }
    memcpy(out, r, sizeof(r));
}

static bool InvertAffine(const float m[16], float out[16]) {
    const float a = m[0], b = m[4], c = m[8];
    const float d = m[1], e = m[5], f = m[9];
    const float g = m[2], h = m[6], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / det;
    float r[16] = {};
    r[0] = (e * i - f * h) * inv;
    r[4] = (c * h - b * i) * inv;
    r[8] = (b * f - c * e) * inv;
    r[1] = (f * g - d * i) * inv;
    r[5] = (a * i - c * g) * inv;
    r[9] = (c * d - a * f) * inv;
    r[2] = (d * h - e * g) * inv;
    r[6] = (b * g - a * h) * inv;
    r[10] = (a * e - b * d) * inv;
    for (int row = 0; row < 3; row++) {
        r[12 + row] = -(r[row] * m[12] + r[4 + row] * m[13] + r[8 + row] * m[14]);
    }
    r[15] = 1.0f;
    memcpy(out, r, sizeof(r));
    return true;
}

static void Rotate(const XrQuaternionf& q, const float v[3], float out[3]) {
    const float t[3] = { 2.0f * (q.y * v[2] - q.z * v[1]), 2.0f * (q.z * v[0] - q.x * v[2]),
                         2.0f * (q.x * v[1] - q.y * v[0]) };
    out[0] = v[0] + q.w * t[0] + (q.y * t[2] - q.z * t[1]);
    out[1] = v[1] + q.w * t[1] + (q.z * t[0] - q.x * t[2]);
    out[2] = v[2] + q.w * t[2] + (q.x * t[1] - q.y * t[0]);
}

template <typename T> static T* Proc(XrInstance instance, const char* name, T** out) {
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, (PFN_xrVoidFunction*)out))) {
        *out = nullptr;
    }
    return *out;
}

bool XrVirtualKeyboard::Wanted(const std::vector<XrExtensionProperties>& extensions) {
    bool keyboard = false;
    bool model = false;
    for (const XrExtensionProperties& extension : extensions) {
        keyboard = keyboard || strcmp(extension.extensionName, XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME) == 0;
        model = model || strcmp(extension.extensionName, XR_FB_RENDER_MODEL_EXTENSION_NAME) == 0;
    }
    return keyboard && model;
}

void XrVirtualKeyboard::AddExtensions(std::vector<const char*>* enabled) {
    enabled->push_back(XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME);
    enabled->push_back(XR_FB_RENDER_MODEL_EXTENSION_NAME);
}

bool XrVirtualKeyboard::Start(XrInstance instance, XrSystemId system, XrSession session, XrSpace space) {
    mInstance = instance;
    mSession = session;
    mSpace = space;

    XrSystemVirtualKeyboardPropertiesMETA keyboardProperties{ XR_TYPE_SYSTEM_VIRTUAL_KEYBOARD_PROPERTIES_META };
    XrSystemProperties systemProperties{ XR_TYPE_SYSTEM_PROPERTIES };
    systemProperties.next = &keyboardProperties;
    if (XR_FAILED(xrGetSystemProperties(instance, system, &systemProperties)) ||
        keyboardProperties.supportsVirtualKeyboard != XR_TRUE) {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "the system has no virtual keyboard");
        return false;
    }

    if (!Proc(instance, "xrCreateVirtualKeyboardMETA", &mCreate) ||
        !Proc(instance, "xrDestroyVirtualKeyboardMETA", &mDestroy) ||
        !Proc(instance, "xrCreateVirtualKeyboardSpaceMETA", &mCreateSpace) ||
        !Proc(instance, "xrSuggestVirtualKeyboardLocationMETA", &mSuggestLocation) ||
        !Proc(instance, "xrGetVirtualKeyboardScaleMETA", &mGetScale) ||
        !Proc(instance, "xrSetVirtualKeyboardModelVisibilityMETA", &mSetVisibility) ||
        !Proc(instance, "xrGetVirtualKeyboardModelAnimationStatesMETA", &mGetAnimationStates) ||
        !Proc(instance, "xrGetVirtualKeyboardDirtyTexturesMETA", &mGetDirtyTextures) ||
        !Proc(instance, "xrGetVirtualKeyboardTextureDataMETA", &mGetTextureData) ||
        !Proc(instance, "xrSendVirtualKeyboardInputMETA", &mSendInput) ||
        !Proc(instance, "xrChangeVirtualKeyboardTextContextMETA", &mChangeTextContext) ||
        !Proc(instance, "xrEnumerateRenderModelPathsFB", &mEnumerateModelPaths) ||
        !Proc(instance, "xrGetRenderModelPropertiesFB", &mGetModelProperties) ||
        !Proc(instance, "xrLoadRenderModelFB", &mLoadModel)) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "virtual keyboard: missing entry points");
        return false;
    }

    XrVirtualKeyboardCreateInfoMETA createInfo{ XR_TYPE_VIRTUAL_KEYBOARD_CREATE_INFO_META };
    XrResult result = mCreate(session, &createInfo, &mKeyboard);
    if (XR_FAILED(result)) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "virtual keyboard: create failed (%d)", (int)result);
        mKeyboard = XR_NULL_HANDLE;
        return false;
    }

    XrVirtualKeyboardSpaceCreateInfoMETA spaceInfo{ XR_TYPE_VIRTUAL_KEYBOARD_SPACE_CREATE_INFO_META };
    spaceInfo.locationType = XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_CUSTOM_META;
    spaceInfo.space = space;
    spaceInfo.poseInSpace.orientation.w = 1.0f;
    result = mCreateSpace(session, mKeyboard, &spaceInfo, &mKeyboardSpace);
    if (XR_FAILED(result)) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "virtual keyboard: space failed (%d)", (int)result);
        Stop();
        return false;
    }

    if (!StartRenderer()) {
        Stop();
        return false;
    }
    Poll();

    XrVirtualKeyboardModelVisibilitySetInfoMETA visibility{ XR_TYPE_VIRTUAL_KEYBOARD_MODEL_VISIBILITY_SET_INFO_META };
    visibility.visible = XR_FALSE;
    mSetVisibility(mKeyboard, &visibility);
    return true;
}

XrVirtualKeyboard::~XrVirtualKeyboard() {
    if (mLoader.joinable()) {
        mLoader.join();
    }
}

void XrVirtualKeyboard::Poll() {
    if (mKeyboard == XR_NULL_HANDLE || mModelLoaded) {
        return;
    }
    const Load state = mLoad.load(std::memory_order_acquire);
    if (state == Load::Running) {
        return;
    }
    if (mLoader.joinable()) {
        mLoader.join();
    }
    if (state == Load::Done) {
        FinishModel();
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "virtual keyboard: model ready after %d tries", mLoadTries);
        return;
    }
    if (state == Load::Failed || mLoadTries >= LOAD_TRIES) {
        if (mLoadTries >= 0) {
            __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "virtual keyboard: no model after %d tries", mLoadTries);
            mLoadTries = -1 - LOAD_TRIES;
        }
        return;
    }
    if (mLoadWait > 0) {
        mLoadWait--;
        return;
    }
    mLoadWait = LOAD_INTERVAL;
    mLoadTries++;
    mLoad.store(Load::Running, std::memory_order_release);
    mLoader = std::thread([this]() { mLoad.store(LoadModel(), std::memory_order_release); });
}

XrVirtualKeyboard::Load XrVirtualKeyboard::LoadModel() {
    uint32_t count = 0;
    if (XR_FAILED(mEnumerateModelPaths(mSession, 0, &count, nullptr))) {
        return Load::Retry;
    }
    std::vector<XrRenderModelPathInfoFB> paths(count, { XR_TYPE_RENDER_MODEL_PATH_INFO_FB });
    if (XR_FAILED(mEnumerateModelPaths(mSession, count, &count, paths.data()))) {
        return Load::Retry;
    }

    XrPath wanted = XR_NULL_PATH;
    xrStringToPath(mInstance, MODEL_PATH, &wanted);
    bool listed = false;
    for (const XrRenderModelPathInfoFB& info : paths) {
        listed = listed || info.path == wanted;
    }
    if (!listed) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "virtual keyboard: no model %s among %u", MODEL_PATH, count);
        return Load::Failed;
    }

    XrRenderModelCapabilitiesRequestFB request{ XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB };
    request.flags = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB;
    XrRenderModelPropertiesFB properties{ XR_TYPE_RENDER_MODEL_PROPERTIES_FB };
    properties.next = &request;
    if (XR_FAILED(mGetModelProperties(mSession, wanted, &properties)) ||
        properties.modelKey == XR_NULL_RENDER_MODEL_KEY_FB) {
        return Load::Retry;
    }

    XrRenderModelLoadInfoFB loadInfo{ XR_TYPE_RENDER_MODEL_LOAD_INFO_FB };
    loadInfo.modelKey = properties.modelKey;
    XrRenderModelBufferFB buffer{ XR_TYPE_RENDER_MODEL_BUFFER_FB };
    if (XR_FAILED(mLoadModel(mSession, &loadInfo, &buffer)) || buffer.bufferCountOutput == 0) {
        return Load::Retry;
    }
    std::vector<uint8_t> glb(buffer.bufferCountOutput);
    buffer.buffer = glb.data();
    buffer.bufferCapacityInput = (uint32_t)glb.size();
    if (XR_FAILED(mLoadModel(mSession, &loadInfo, &buffer))) {
        return Load::Retry;
    }

    const auto started = std::chrono::steady_clock::now();
    const bool parsed = ParseModel(glb);
    const long ms =
        (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    mModelName = properties.modelName;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "virtual keyboard: model %s v%u, %zu bytes, parsed in %ld ms off the render thread: %zu nodes, "
                        "%zu meshes, %zu images (%zu live), %zu animations, collision %d [%.3f %.3f %.3f]..[%.3f %.3f "
                        "%.3f]",
                        properties.modelName, properties.modelVersion, glb.size(), ms, mNodes.size(), mMeshes.size(),
                        mImages.size(), mDynamicTextures.size(), mAnimations.size(), mCollisionNode, mCollisionMin[0],
                        mCollisionMin[1], mCollisionMin[2], mCollisionMax[0], mCollisionMax[1], mCollisionMax[2]);
    return parsed ? Load::Done : Load::Failed;
}

struct XrVirtualKeyboard::Accessor {
    static bool Read(const nlohmann::json& gltf, const std::vector<uint8_t>& bin, int index, std::vector<float>* out,
                     int* components) {
        const nlohmann::json& accessors = gltf["accessors"];
        if (index < 0 || index >= (int)accessors.size()) {
            return false;
        }
        const nlohmann::json& accessor = accessors[index];
        const std::string type = accessor.value("type", "SCALAR");
        const int count = accessor.value("count", 0);
        const int componentType = accessor.value("componentType", 5126);
        const bool normalized = accessor.value("normalized", false);
        int n = 1;
        if (type == "VEC2") {
            n = 2;
        } else if (type == "VEC3") {
            n = 3;
        } else if (type == "VEC4" || type == "MAT2") {
            n = 4;
        } else if (type == "MAT3") {
            n = 9;
        } else if (type == "MAT4") {
            n = 16;
        }
        int size = 4;
        if (componentType == 5120 || componentType == 5121) {
            size = 1;
        } else if (componentType == 5122 || componentType == 5123) {
            size = 2;
        }
        *components = n;
        out->assign((size_t)count * n, 0.0f);

        auto element = [&](const uint8_t* p) -> float {
            switch (componentType) {
                case 5120: {
                    const float v = (float)*(const int8_t*)p;
                    return normalized ? std::max(v / 127.0f, -1.0f) : v;
                }
                case 5121: {
                    const float v = (float)*p;
                    return normalized ? v / 255.0f : v;
                }
                case 5122: {
                    int16_t s;
                    memcpy(&s, p, 2);
                    return normalized ? std::max((float)s / 32767.0f, -1.0f) : (float)s;
                }
                case 5123: {
                    uint16_t s;
                    memcpy(&s, p, 2);
                    return normalized ? (float)s / 65535.0f : (float)s;
                }
                case 5125: {
                    uint32_t s;
                    memcpy(&s, p, 4);
                    return (float)s;
                }
                default: {
                    float f;
                    memcpy(&f, p, 4);
                    return f;
                }
            }
        };
        auto view = [&](int viewIndex, size_t extra, size_t* offset, size_t* stride) -> bool {
            const nlohmann::json& views = gltf["bufferViews"];
            if (viewIndex < 0 || viewIndex >= (int)views.size()) {
                return false;
            }
            *offset = views[viewIndex].value("byteOffset", (size_t)0) + extra;
            *stride = views[viewIndex].value("byteStride", (size_t)0);
            return true;
        };

        if (accessor.contains("bufferView")) {
            size_t offset = 0;
            size_t stride = 0;
            if (!view(accessor["bufferView"].get<int>(), accessor.value("byteOffset", (size_t)0), &offset, &stride)) {
                return false;
            }
            if (stride == 0) {
                stride = (size_t)size * n;
            }
            if (count > 0 && offset + stride * (count - 1) + (size_t)size * n > bin.size()) {
                return false;
            }
            for (int i = 0; i < count; i++) {
                for (int c = 0; c < n; c++) {
                    (*out)[(size_t)i * n + c] = element(bin.data() + offset + stride * i + (size_t)size * c);
                }
            }
        }

        if (accessor.contains("sparse")) {
            const nlohmann::json& sparse = accessor["sparse"];
            const int sparseCount = sparse.value("count", 0);
            const nlohmann::json& indices = sparse["indices"];
            const nlohmann::json& values = sparse["values"];
            size_t indexOffset = 0;
            size_t valueOffset = 0;
            size_t ignored = 0;
            if (!view(indices["bufferView"].get<int>(), indices.value("byteOffset", (size_t)0), &indexOffset,
                      &ignored) ||
                !view(values["bufferView"].get<int>(), values.value("byteOffset", (size_t)0), &valueOffset, &ignored)) {
                return false;
            }
            const int indexType = indices.value("componentType", 5125);
            for (int i = 0; i < sparseCount; i++) {
                uint32_t target = 0;
                if (indexType == 5121) {
                    target = bin[indexOffset + i];
                } else if (indexType == 5123) {
                    uint16_t s;
                    memcpy(&s, bin.data() + indexOffset + (size_t)i * 2, 2);
                    target = s;
                } else {
                    memcpy(&target, bin.data() + indexOffset + (size_t)i * 4, 4);
                }
                if ((int)target >= count) {
                    continue;
                }
                for (int c = 0; c < n; c++) {
                    (*out)[(size_t)target * n + c] =
                        element(bin.data() + valueOffset + ((size_t)i * n + c) * (size_t)size);
                }
            }
        }
        return true;
    }
};

static bool ParseTextureUri(const std::string& uri, uint64_t* id, uint32_t* width, uint32_t* height) {
    if (uri.rfind(TEXTURE_URI, 0) != 0) {
        return false;
    }
    unsigned long long value = 0;
    unsigned w = 0;
    unsigned h = 0;
    char format[16] = "";
    if (sscanf(uri.c_str() + strlen(TEXTURE_URI), "%llu?w=%u&h=%u&fmt=%15s", &value, &w, &h, format) != 4 ||
        strcmp(format, "RGBA32") != 0 || w == 0 || h == 0) {
        return false;
    }
    *id = value;
    *width = w;
    *height = h;
    return true;
}

bool XrVirtualKeyboard::ParseModel(const std::vector<uint8_t>& glb) {
    if (glb.size() < 20) {
        return false;
    }
    uint32_t header[3];
    memcpy(header, glb.data(), sizeof(header));
    if (header[0] != 0x46546C67 || header[1] != 2) {
        return false;
    }

    std::string json;
    std::vector<uint8_t> bin;
    size_t offset = 12;
    while (offset + 8 <= glb.size()) {
        uint32_t chunk[2];
        memcpy(chunk, glb.data() + offset, sizeof(chunk));
        offset += 8;
        if (offset + chunk[0] > glb.size()) {
            return false;
        }
        if (chunk[1] == 0x4E4F534A) {
            json.assign((const char*)glb.data() + offset, chunk[0]);
        } else if (chunk[1] == 0x004E4942) {
            bin.assign(glb.begin() + offset, glb.begin() + offset + chunk[0]);
        }
        offset += (chunk[0] + 3) & ~3u;
    }

    nlohmann::json gltf = nlohmann::json::parse(json, nullptr, false);
    if (gltf.is_discarded()) {
        return false;
    }

    std::vector<int> textureOfImage;
    if (gltf.contains("images")) {
        for (const nlohmann::json& image : gltf["images"]) {
            Image entry;
            const std::string uri = image.value("uri", "");
            if (ParseTextureUri(uri, &entry.id, &entry.width, &entry.height)) {
                entry.live = true;
                mDynamicTextures[entry.id] = (int)mImages.size();
            } else {
                int w = 0;
                int h = 0;
                int channels = 0;
                stbi_uc* pixels = nullptr;
                if (image.contains("bufferView")) {
                    const nlohmann::json& view = gltf["bufferViews"][image["bufferView"].get<int>()];
                    const size_t start = view.value("byteOffset", (size_t)0);
                    const size_t length = view.value("byteLength", (size_t)0);
                    if (start + length <= bin.size()) {
                        pixels = stbi_load_from_memory(bin.data() + start, (int)length, &w, &h, &channels, 4);
                    }
                }
                if (pixels == nullptr) {
                    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "virtual keyboard: image '%s' (%s) not decoded",
                                        image.value("name", "").c_str(), image.value("mimeType", uri).c_str());
                    entry.pixels.assign(4, 255);
                } else {
                    entry.width = (uint32_t)w;
                    entry.height = (uint32_t)h;
                    entry.pixels.assign(pixels, pixels + (size_t)w * h * 4);
                    stbi_image_free(pixels);
                }
            }
            textureOfImage.push_back((int)mImages.size());
            mImages.push_back(std::move(entry));
        }
    }

    if (gltf.contains("textures")) {
        for (const nlohmann::json& texture : gltf["textures"]) {
            int source = texture.value("source", -1);
            if (texture.contains("extensions") && texture["extensions"].contains("KHR_texture_basisu")) {
                source = texture["extensions"]["KHR_texture_basisu"].value("source", source);
            }
            mTextureOfGltfTexture.push_back(source >= 0 && source < (int)textureOfImage.size() ? textureOfImage[source]
                                                                                               : -1);
        }
    }

    if (gltf.contains("materials")) {
        for (const nlohmann::json& material : gltf["materials"]) {
            Material entry;
            if (material.contains("pbrMetallicRoughness")) {
                const nlohmann::json& pbr = material["pbrMetallicRoughness"];
                if (pbr.contains("baseColorFactor")) {
                    for (int i = 0; i < 4; i++) {
                        entry.baseColor[i] = pbr["baseColorFactor"][i].get<float>();
                    }
                }
                if (pbr.contains("baseColorTexture")) {
                    const int index = pbr["baseColorTexture"].value("index", -1);
                    if (index >= 0 && index < (int)mTextureOfGltfTexture.size()) {
                        entry.texture = mTextureOfGltfTexture[index];
                    }
                }
            }
            mMaterials.push_back(entry);
        }
    }

    int components = 0;
    std::vector<float> values;
    if (gltf.contains("meshes")) {
        for (const nlohmann::json& mesh : gltf["meshes"]) {
            Mesh entry;
            for (const nlohmann::json& primitive : mesh["primitives"]) {
                if (primitive.value("mode", 4) != 4 || !primitive["attributes"].contains("POSITION")) {
                    continue;
                }
                Primitive p;
                p.material = primitive.value("material", -1);
                const nlohmann::json& attributes = primitive["attributes"];
                Accessor::Read(gltf, bin, attributes["POSITION"].get<int>(), &p.position, &components);
                const size_t vertexCount = p.position.size() / 3;
                if (attributes.contains("TEXCOORD_0")) {
                    Accessor::Read(gltf, bin, attributes["TEXCOORD_0"].get<int>(), &p.uv, &components);
                }
                p.uv.resize(vertexCount * 2, 0.0f);
                if (primitive.contains("indices")) {
                    Accessor::Read(gltf, bin, primitive["indices"].get<int>(), &values, &components);
                    p.indices.assign(values.begin(), values.end());
                } else {
                    for (uint32_t i = 0; i < vertexCount; i++) {
                        p.indices.push_back(i);
                    }
                }
                if (primitive.contains("targets")) {
                    for (const nlohmann::json& target : primitive["targets"]) {
                        std::vector<float> position;
                        std::vector<float> uv;
                        if (target.contains("POSITION")) {
                            Accessor::Read(gltf, bin, target["POSITION"].get<int>(), &position, &components);
                        }
                        if (target.contains("TEXCOORD_0")) {
                            Accessor::Read(gltf, bin, target["TEXCOORD_0"].get<int>(), &uv, &components);
                        }
                        p.targetPosition.push_back(position);
                        p.targetUv.push_back(uv);
                    }
                }
                p.morphedPosition = p.position;
                p.morphedUv = p.uv;
                entry.primitives.push_back(std::move(p));
            }
            if (mesh.contains("weights")) {
                entry.weights = mesh["weights"].get<std::vector<float>>();
            }
            mMeshes.push_back(std::move(entry));
        }
    }

    if (gltf.contains("nodes")) {
        mNodes.resize(gltf["nodes"].size());
        for (size_t i = 0; i < mNodes.size(); i++) {
            const nlohmann::json& node = gltf["nodes"][i];
            Node& entry = mNodes[i];
            entry.name = node.value("name", "");
            entry.mesh = node.value("mesh", -1);
            if (node.contains("matrix")) {
                entry.hasMatrix = true;
                for (int k = 0; k < 16; k++) {
                    entry.matrix[k] = node["matrix"][k].get<float>();
                }
            }
            for (int k = 0; k < 3 && node.contains("translation"); k++) {
                entry.translation[k] = node["translation"][k].get<float>();
            }
            for (int k = 0; k < 4 && node.contains("rotation"); k++) {
                entry.rotation[k] = node["rotation"][k].get<float>();
            }
            for (int k = 0; k < 3 && node.contains("scale"); k++) {
                entry.scale[k] = node["scale"][k].get<float>();
            }
            if (node.contains("children")) {
                entry.children = node["children"].get<std::vector<int>>();
            }
            if (node.contains("weights")) {
                entry.weights = node["weights"].get<std::vector<float>>();
            } else if (entry.mesh >= 0 && entry.mesh < (int)mMeshes.size()) {
                entry.weights = mMeshes[entry.mesh].weights;
                if (entry.weights.empty() && !mMeshes[entry.mesh].primitives.empty()) {
                    entry.weights.assign(mMeshes[entry.mesh].primitives[0].targetPosition.size(), 0.0f);
                }
            }
            if (entry.name == "collision") {
                mCollisionNode = (int)i;
            }
        }
        for (size_t i = 0; i < mNodes.size(); i++) {
            for (int child : mNodes[i].children) {
                if (child >= 0 && child < (int)mNodes.size()) {
                    mNodes[child].parent = (int)i;
                }
            }
        }
    }

    if (gltf.contains("animations")) {
        bool firstTimeline = true;
        for (const nlohmann::json& animation : gltf["animations"]) {
            Animation entry;
            for (const nlohmann::json& sampler : animation["samplers"]) {
                Sampler s;
                int inputComponents = 0;
                Accessor::Read(gltf, bin, sampler["input"].get<int>(), &s.times, &inputComponents);
                Accessor::Read(gltf, bin, sampler["output"].get<int>(), &s.values, &s.components);
                s.step = sampler.value("interpolation", "LINEAR") == "STEP";
                if (!s.times.empty()) {
                    if (firstTimeline) {
                        mAnimationStart = s.times.front();
                        mAnimationEnd = s.times.back();
                        firstTimeline = false;
                    } else {
                        mAnimationStart = std::min(mAnimationStart, s.times.front());
                        mAnimationEnd = std::max(mAnimationEnd, s.times.back());
                    }
                }
                entry.samplers.push_back(std::move(s));
            }
            for (const nlohmann::json& channel : animation["channels"]) {
                Channel c;
                c.sampler = channel.value("sampler", -1);
                const nlohmann::json& target = channel["target"];
                c.node = target.value("node", -1);
                const std::string path = target.value("path", "");
                if (path == "translation") {
                    c.path = Path::Translation;
                } else if (path == "rotation") {
                    c.path = Path::Rotation;
                } else if (path == "scale") {
                    c.path = Path::Scale;
                } else if (path == "weights") {
                    c.path = Path::Weights;
                } else {
                    continue;
                }
                if (channel.contains("extras")) {
                    c.additiveWeightIndex = channel["extras"].value("additiveWeightIndex", -1);
                }
                if (c.sampler < 0 || c.sampler >= (int)entry.samplers.size() || c.node < 0 ||
                    c.node >= (int)mNodes.size()) {
                    continue;
                }
                entry.channels.push_back(c);
            }
            mAnimations.push_back(std::move(entry));
        }
    }

    UpdateTransforms();
    if (mCollisionNode >= 0 && mNodes[mCollisionNode].mesh >= 0) {
        bool first = true;
        for (const Primitive& p : mMeshes[mNodes[mCollisionNode].mesh].primitives) {
            for (size_t v = 0; v + 2 < p.position.size(); v += 3) {
                for (int k = 0; k < 3; k++) {
                    mCollisionMin[k] = first ? p.position[v + k] : std::min(mCollisionMin[k], p.position[v + k]);
                    mCollisionMax[k] = first ? p.position[v + k] : std::max(mCollisionMax[k], p.position[v + k]);
                }
                first = false;
            }
        }
    }
    return mCollisionNode >= 0 && !mNodes.empty();
}

bool XrVirtualKeyboard::StartRenderer() {
    static const char* VERTEX = "#version 300 es\n"
                                "uniform mat4 uMvp;\n"
                                "layout(location = 0) in vec3 aPosition;\n"
                                "layout(location = 1) in vec2 aUv;\n"
                                "out vec2 vUv;\n"
                                "void main() {\n"
                                "    vUv = aUv;\n"
                                "    gl_Position = uMvp * vec4(aPosition, 1.0);\n"
                                "}\n";
    static const char* FRAGMENT = "#version 300 es\n"
                                  "precision mediump float;\n"
                                  "uniform sampler2D uTex;\n"
                                  "uniform vec4 uBaseColor;\n"
                                  "in vec2 vUv;\n"
                                  "out vec4 oColor;\n"
                                  "void main() {\n"
                                  "    vec4 c = texture(uTex, vUv);\n"
                                  "    float a = c.a * uBaseColor.a;\n"
                                  "    if (a < 0.004) {\n"
                                  "        discard;\n"
                                  "    }\n"
                                  "    oColor = vec4(c.rgb * uBaseColor.rgb * a, a);\n"
                                  "}\n";
    auto compile = [](GLenum type, const char* source) -> GLuint {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[512] = "";
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "virtual keyboard shader: %s", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };
    const GLuint vertex = compile(GL_VERTEX_SHADER, VERTEX);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, FRAGMENT);
    if (vertex == 0 || fragment == 0) {
        return false;
    }
    mProgram = glCreateProgram();
    glAttachShader(mProgram, vertex);
    glAttachShader(mProgram, fragment);
    glLinkProgram(mProgram);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(mProgram);
        mProgram = 0;
        return false;
    }
    mMvpLoc = glGetUniformLocation(mProgram, "uMvp");
    mBaseColorLoc = glGetUniformLocation(mProgram, "uBaseColor");
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glUseProgram(mProgram);
    glUniform1i(glGetUniformLocation(mProgram, "uTex"), 0);
    glUseProgram((GLuint)program);

    GLint vao = 0;
    GLint arrayBuffer = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    GLint texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    const uint8_t white[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &mWhite);
    glBindTexture(GL_TEXTURE_2D, mWhite);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glBindTexture(GL_TEXTURE_2D, (GLuint)texture);

    glGenVertexArrays(1, &mVao);
    glGenBuffers(1, &mVbo);
    glGenBuffers(1, &mIbo);
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)(3 * sizeof(float)));
    glBindVertexArray((GLuint)vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)arrayBuffer);
    return true;
}

void XrVirtualKeyboard::Stop() {
    if (mLoader.joinable()) {
        mLoader.join();
    }
    mLoad.store(Load::Idle);
    for (Texture& texture : mTextures) {
        if (texture.gl != 0) {
            glDeleteTextures(1, &texture.gl);
        }
    }
    mTextures.clear();
    mDynamicTextures.clear();
    if (mProgram != 0) {
        glDeleteProgram(mProgram);
        mProgram = 0;
    }
    if (mVao != 0) {
        glDeleteVertexArrays(1, &mVao);
        mVao = 0;
    }
    if (mWhite != 0) {
        glDeleteTextures(1, &mWhite);
        mWhite = 0;
    }
    if (mVbo != 0) {
        glDeleteBuffers(1, &mVbo);
        mVbo = 0;
    }
    if (mIbo != 0) {
        glDeleteBuffers(1, &mIbo);
        mIbo = 0;
    }
    if (mKeyboardSpace != XR_NULL_HANDLE) {
        xrDestroySpace(mKeyboardSpace);
        mKeyboardSpace = XR_NULL_HANDLE;
    }
    if (mKeyboard != XR_NULL_HANDLE && mDestroy != nullptr) {
        mDestroy(mKeyboard);
    }
    mKeyboard = XR_NULL_HANDLE;
    mModelLoaded = false;
    mShown = false;
    mWantVisible = false;
    mNodes.clear();
    mMeshes.clear();
    mMaterials.clear();
    mAnimations.clear();
    mTextureOfGltfTexture.clear();
    mImages.clear();
    mSlots.clear();
    mBatches.clear();
    mDynamicTextures.clear();
    mLoadTries = 0;
    mLoadWait = 0;
}

void XrVirtualKeyboard::SetVisible(bool visible, const char* textContext) {
    if (!Available() || visible == mWantVisible) {
        return;
    }
    mWantVisible = visible;
    if (visible) {
        mSuggestPending = true;
        mPoseValid = false;
        XrVirtualKeyboardTextContextChangeInfoMETA context{ XR_TYPE_VIRTUAL_KEYBOARD_TEXT_CONTEXT_CHANGE_INFO_META };
        context.textContext = textContext != nullptr ? textContext : "";
        mChangeTextContext(mKeyboard, &context);
    } else {
        mShown = false;
    }
    XrVirtualKeyboardModelVisibilitySetInfoMETA info{ XR_TYPE_VIRTUAL_KEYBOARD_MODEL_VISIBILITY_SET_INFO_META };
    info.visible = visible ? XR_TRUE : XR_FALSE;
    const XrResult result = mSetVisibility(mKeyboard, &info);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "virtual keyboard: %s (%d)", visible ? "show" : "hide", (int)result);
}

bool XrVirtualKeyboard::HandleEvent(const XrEventDataBuffer& event) {
    switch (event.type) {
        case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_COMMIT_TEXT_META:
            mText += ((const XrEventDataVirtualKeyboardCommitTextMETA*)&event)->text;
#ifdef ENABLE_DEBUG_TOOLS
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "virtual keyboard: commit '%s'",
                                ((const XrEventDataVirtualKeyboardCommitTextMETA*)&event)->text);
#endif
            return true;
        case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_BACKSPACE_META:
            mBackspaces++;
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "virtual keyboard: backspace");
            return true;
        case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_ENTER_META:
            mEnter = true;
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "virtual keyboard: enter");
            return true;
        case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_SHOWN_META:
            mShown = mWantVisible;
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "virtual keyboard: shown");
            return true;
        case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_HIDDEN_META:
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "virtual keyboard: hidden");
            if (mWantVisible) {
                mClosed = true;
            }
            mWantVisible = false;
            mShown = false;
            return true;
        default:
            return false;
    }
}

float XrVirtualKeyboard::Aspect() const {
    const float width = mVisualMax[0] - mVisualMin[0];
    return Available() && width > 1e-4f ? (mVisualMax[1] - mVisualMin[1]) / width : 0.0f;
}

void XrVirtualKeyboard::Place(const XrPosef& pose, float width) {
    const float modelWidth = mVisualMax[0] - mVisualMin[0];
    if (!Available() || modelWidth <= 1e-4f) {
        return;
    }
    const float scale = width / modelWidth;
    const float center[3] = { 0.5f * (mVisualMin[0] + mVisualMax[0]) * scale,
                              0.5f * (mVisualMin[1] + mVisualMax[1]) * scale,
                              0.5f * (mVisualMin[2] + mVisualMax[2]) * scale };
    float turned[3];
    Rotate(pose.orientation, center, turned);
    XrPosef wanted = pose;
    wanted.position = { pose.position.x - turned[0], pose.position.y - turned[1], pose.position.z - turned[2] };

    const float moved =
        fabsf(wanted.position.x - mWantedPose.position.x) + fabsf(wanted.position.y - mWantedPose.position.y) +
        fabsf(wanted.position.z - mWantedPose.position.z) + fabsf(wanted.orientation.x - mWantedPose.orientation.x) +
        fabsf(wanted.orientation.y - mWantedPose.orientation.y) +
        fabsf(wanted.orientation.z - mWantedPose.orientation.z) +
        fabsf(wanted.orientation.w - mWantedPose.orientation.w);
    if (moved > 1e-3f || fabsf(scale - mWantedScale) > 1e-3f) {
        mWantedPose = wanted;
        mWantedScale = scale;
        mSuggestPending = true;
    }
}

void XrVirtualKeyboard::Suggest() {
    XrVirtualKeyboardLocationInfoMETA location{ XR_TYPE_VIRTUAL_KEYBOARD_LOCATION_INFO_META };
    location.locationType = XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_CUSTOM_META;
    location.space = mSpace;
    location.poseInSpace = mWantedPose;
    location.scale = mWantedScale;
    const XrResult result = mSuggestLocation(mKeyboard, &location);
    if (XR_FAILED(result)) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "virtual keyboard: location refused (%d)", (int)result);
    }
    mSuggestPending = false;
}

bool XrVirtualKeyboard::Locate(XrTime time) {
    XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(mKeyboardSpace, mSpace, time, &location)) ||
        (location.locationFlags & needed) != needed) {
        return false;
    }
    float scale = mScale;
    if (XR_SUCCEEDED(mGetScale(mKeyboard, &scale)) && scale > 0.0f) {
        mScale = scale;
    }
    mPose = location.pose;
    return true;
}

void XrVirtualKeyboard::Update(XrTime time) {
    if (!Available() || !mWantVisible) {
        return;
    }
    if (mSuggestPending) {
        Suggest();
    }
    mPoseValid = Locate(time);
    UpdateTextures();
    UpdateAnimations();
    if (mTransformsDirty) {
        UpdateTransforms();
    }
    if (mVerticesDirty) {
        RefreshVertices();
    }
}

void XrVirtualKeyboard::UpdateTextures() {
    uint32_t count = 0;
    if (XR_FAILED(mGetDirtyTextures(mKeyboard, 0, &count, nullptr)) || count == 0) {
        return;
    }
    std::vector<uint64_t> ids(count);
    if (XR_FAILED(mGetDirtyTextures(mKeyboard, count, &count, ids.data()))) {
        return;
    }
    GLint savedTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture);
    for (uint64_t id : ids) {
        auto found = mDynamicTextures.find(id);
        if (found == mDynamicTextures.end()) {
            continue;
        }
        const Texture& texture = mTextures[found->second];
        XrVirtualKeyboardTextureDataMETA data{ XR_TYPE_VIRTUAL_KEYBOARD_TEXTURE_DATA_META };
        if (XR_FAILED(mGetTextureData(mKeyboard, id, &data)) || data.bufferCountOutput == 0) {
            continue;
        }
        mTextureBuffer.resize(data.bufferCountOutput);
        data.bufferCapacityInput = data.bufferCountOutput;
        data.buffer = mTextureBuffer.data();
        if (XR_FAILED(mGetTextureData(mKeyboard, id, &data)) || data.textureWidth != texture.width ||
            data.textureHeight != texture.height) {
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, texture.gl);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture.width, texture.height, GL_RGBA, GL_UNSIGNED_BYTE,
                        mTextureBuffer.data());
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTexture);
}

void XrVirtualKeyboard::UpdateAnimations() {
    XrVirtualKeyboardModelAnimationStatesMETA states{ XR_TYPE_VIRTUAL_KEYBOARD_MODEL_ANIMATION_STATES_META };
    if (XR_FAILED(mGetAnimationStates(mKeyboard, &states)) || states.stateCountOutput == 0) {
        return;
    }
    mAnimationStates.assign(states.stateCountOutput, { XR_TYPE_VIRTUAL_KEYBOARD_ANIMATION_STATE_META });
    states.stateCapacityInput = states.stateCountOutput;
    states.states = mAnimationStates.data();
    if (XR_FAILED(mGetAnimationStates(mKeyboard, &states))) {
        return;
    }
    for (uint32_t i = 0; i < states.stateCountOutput; i++) {
        ApplyAnimation(mAnimationStates[i].animationIndex, mAnimationStates[i].fraction);
    }
}

void XrVirtualKeyboard::ApplyAnimation(int index, float fraction) {
    if (index < 0 || index >= (int)mAnimations.size()) {
        return;
    }
    const float time = (mAnimationEnd - mAnimationStart) * std::clamp(fraction, 0.0f, 1.0f);
    const Animation& animation = mAnimations[index];
    for (const Channel& channel : animation.channels) {
        const Sampler& sampler = animation.samplers[channel.sampler];
        const int keys = (int)sampler.times.size();
        if (keys == 0) {
            continue;
        }
        int frame = 0;
        float t = 0.0f;
        if (keys == 1 || time <= sampler.times.front()) {
            frame = 0;
            t = 0.0f;
        } else if (time >= sampler.times.back()) {
            frame = keys - 2;
            t = 1.0f;
        } else {
            while (frame + 1 < keys && time >= sampler.times[frame + 1]) {
                frame++;
            }
            t = (time - sampler.times[frame]) / (sampler.times[frame + 1] - sampler.times[frame]);
        }
        if (sampler.step) {
            t = t >= 1.0f ? 1.0f : 0.0f;
        }
        const int next = std::min(frame + 1, keys - 1);
        const int width = (int)(sampler.values.size() / keys);
        auto value = [&](int k) {
            return sampler.values[(size_t)frame * width + k] +
                   (sampler.values[(size_t)next * width + k] - sampler.values[(size_t)frame * width + k]) * t;
        };

        Node& node = mNodes[channel.node];
        switch (channel.path) {
            case Path::Translation:
                for (int k = 0; k < 3 && k < width; k++) {
                    node.translation[k] = value(k);
                }
                node.hasMatrix = false;
                node.transformDirty = true;
                mTransformsDirty = true;
                break;
            case Path::Scale:
                for (int k = 0; k < 3 && k < width; k++) {
                    node.scale[k] = value(k);
                }
                node.hasMatrix = false;
                node.transformDirty = true;
                mTransformsDirty = true;
                break;
            case Path::Rotation: {
                float q[4] = {};
                float length = 0.0f;
                for (int k = 0; k < 4 && k < width; k++) {
                    q[k] = value(k);
                    length += q[k] * q[k];
                }
                length = sqrtf(length);
                for (int k = 0; k < 4 && length > 0.0f; k++) {
                    node.rotation[k] = q[k] / length;
                }
                node.hasMatrix = false;
                node.transformDirty = true;
                mTransformsDirty = true;
                break;
            }
            case Path::Weights:
                if ((int)node.weights.size() != width) {
                    break;
                }
                if (channel.additiveWeightIndex >= 0 && channel.additiveWeightIndex < width) {
                    node.weights[channel.additiveWeightIndex] += value(channel.additiveWeightIndex);
                } else {
                    for (int k = 0; k < width; k++) {
                        node.weights[k] = value(k);
                    }
                }
                node.geometryDirty = true;
                mVerticesDirty = true;
                break;
        }
    }
}

void XrVirtualKeyboard::UpdateTransforms() {
    std::vector<int> order;
    for (size_t i = 0; i < mNodes.size(); i++) {
        if (mNodes[i].parent < 0) {
            order.push_back((int)i);
        }
    }
    std::vector<char> changed(mNodes.size(), 0);
    for (size_t i = 0; i < order.size(); i++) {
        Node& node = mNodes[order[i]];
        changed[order[i]] = node.transformDirty || (node.parent >= 0 && changed[node.parent] != 0);
        if (changed[order[i]] != 0) {
            float local[16];
            if (node.hasMatrix) {
                memcpy(local, node.matrix, sizeof(local));
            } else {
                FromTrs(node.translation, node.rotation, node.scale, local);
            }
            if (node.parent >= 0) {
                Multiply(mNodes[node.parent].global, local, node.global);
            } else {
                memcpy(node.global, local, sizeof(local));
            }
            node.transformDirty = false;
            node.vertexDirty = true;
            mVerticesDirty = true;
        }
        for (int child : node.children) {
            if (child >= 0 && child < (int)mNodes.size()) {
                order.push_back(child);
            }
        }
    }
    mTransformsDirty = false;
}

void XrVirtualKeyboard::FinishModel() {
    GLint savedTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture);
    for (Image& image : mImages) {
        Texture texture;
        glGenTextures(1, &texture.gl);
        glBindTexture(GL_TEXTURE_2D, texture.gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        const bool mipmaps = !image.live && image.width > 1;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        if (image.live) {
            image.pixels.assign((size_t)image.width * image.height * 4, 0);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     image.pixels.data());
        if (mipmaps) {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        texture.width = image.width;
        texture.height = image.height;
        mTextures.push_back(texture);
        image.pixels.clear();
        image.pixels.shrink_to_fit();
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTexture);
    BuildLayout();
    mModelLoaded = true;
}

void XrVirtualKeyboard::BuildLayout() {
    struct Key {
        int texture;
        const float* baseColor;
    };
    static const float WHITE[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::vector<Key> keys;
    std::vector<std::vector<int>> slotsOfKey;
    auto keyOf = [&](const Primitive& p) {
        const bool known = p.material >= 0 && p.material < (int)mMaterials.size();
        const int texture = known ? mMaterials[p.material].texture : -1;
        const float* baseColor = known ? mMaterials[p.material].baseColor : WHITE;
        for (size_t k = 0; k < keys.size(); k++) {
            if (keys[k].texture == texture && memcmp(keys[k].baseColor, baseColor, sizeof(float) * 4) == 0) {
                return (int)k;
            }
        }
        keys.push_back({ texture, baseColor });
        slotsOfKey.emplace_back();
        return (int)keys.size() - 1;
    };

    mSlots.clear();
    for (size_t n = 0; n < mNodes.size(); n++) {
        Node& node = mNodes[n];
        node.slots.clear();
        if (node.mesh < 0 || node.mesh >= (int)mMeshes.size() || (int)n == mCollisionNode) {
            continue;
        }
        const std::vector<Primitive>& primitives = mMeshes[node.mesh].primitives;
        for (size_t p = 0; p < primitives.size(); p++) {
            Slot slot;
            slot.node = (int)n;
            slot.mesh = node.mesh;
            slot.primitive = (int)p;
            slotsOfKey[keyOf(primitives[p])].push_back((int)mSlots.size());
            mSlots.push_back(slot);
        }
    }

    mIndices.clear();
    mBatches.clear();
    uint32_t vertexCount = 0;
    for (size_t k = 0; k < keys.size(); k++) {
        Batch batch;
        batch.texture = keys[k].texture >= 0 && keys[k].texture < (int)mTextures.size() ? keys[k].texture : -1;
        memcpy(batch.baseColor, keys[k].baseColor, sizeof(batch.baseColor));
        batch.first = (uint32_t)mIndices.size();
        for (int s : slotsOfKey[k]) {
            Slot& slot = mSlots[s];
            const Primitive& p = mMeshes[slot.mesh].primitives[slot.primitive];
            const uint32_t count = (uint32_t)(p.position.size() / 3);
            slot.offset = vertexCount;
            for (uint32_t index : p.indices) {
                if (index < count) {
                    mIndices.push_back(vertexCount + index);
                }
            }
            vertexCount += count;
            mNodes[slot.node].slots.push_back(s);
        }
        batch.count = (uint32_t)mIndices.size() - batch.first;
        mBatches.push_back(batch);
    }
    mVertices.assign((size_t)vertexCount * 5, 0.0f);
    float box[2][3];
    TransformPoint(mNodes[mCollisionNode].global, mCollisionMin, box[0]);
    TransformPoint(mNodes[mCollisionNode].global, mCollisionMax, box[1]);
    for (int k = 0; k < 3; k++) {
        mVisualMin[k] = std::min(box[0][k], box[1][k]);
        mVisualMax[k] = std::max(box[0][k], box[1][k]);
    }
    const float reachX = 0.25f * (mVisualMax[0] - mVisualMin[0]);
    const float reachY = 0.25f * (mVisualMax[1] - mVisualMin[1]);
    const float left = mVisualMin[0] - reachX;
    const float right = mVisualMax[0] + reachX;
    const float bottom = mVisualMin[1] - reachY;
    const float top = mVisualMax[1] + reachY;
    for (const Slot& slot : mSlots) {
        const Primitive& p = mMeshes[slot.mesh].primitives[slot.primitive];
        for (size_t v = 0; v + 2 < p.position.size(); v += 3) {
            float point[3];
            TransformPoint(mNodes[slot.node].global, &p.position[v], point);
            if (point[0] < left || point[0] > right || point[1] < bottom || point[1] > top) {
                continue;
            }
            for (int k = 0; k < 3; k++) {
                mVisualMin[k] = std::min(mVisualMin[k], point[k]);
                mVisualMax[k] = std::max(mVisualMax[k], point[k]);
            }
        }
    }
    for (Node& node : mNodes) {
        node.vertexDirty = true;
    }

    GLint arrayBuffer = 0;
    GLint vao = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBufferData(GL_ARRAY_BUFFER, mVertices.size() * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mIndices.size() * sizeof(uint32_t), mIndices.data(), GL_STATIC_DRAW);
    glBindVertexArray((GLuint)vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)arrayBuffer);
    mVerticesDirty = true;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "virtual keyboard: %u vertices, %zu indices, %zu batches, bounds [%.3f %.3f %.3f]..[%.3f %.3f "
                        "%.3f]",
                        vertexCount, mIndices.size(), mBatches.size(), mVisualMin[0], mVisualMin[1], mVisualMin[2],
                        mVisualMax[0], mVisualMax[1], mVisualMax[2]);
}

void XrVirtualKeyboard::RefreshVertices() {
    size_t low = mVertices.size();
    size_t high = 0;
    for (Node& node : mNodes) {
        if (!node.vertexDirty && !node.geometryDirty) {
            continue;
        }
        for (int s : node.slots) {
            const Slot& slot = mSlots[s];
            Primitive& p = mMeshes[slot.mesh].primitives[slot.primitive];
            if (node.geometryDirty) {
                p.morphedPosition = p.position;
                p.morphedUv = p.uv;
                for (int w = 0; w < (int)node.weights.size() && w < (int)p.targetPosition.size(); w++) {
                    const float weight = node.weights[w];
                    if (weight == 0.0f) {
                        continue;
                    }
                    const std::vector<float>& targetPosition = p.targetPosition[w];
                    const std::vector<float>& targetUv = p.targetUv[w];
                    const size_t positionIndex = (size_t)(w % 2) + (size_t)(w / 2) * 3;
                    if (w < 8 && positionIndex < targetPosition.size() && positionIndex < p.morphedPosition.size()) {
                        p.morphedPosition[positionIndex] += targetPosition[positionIndex] * weight;
                    }
                    const int uvIndex = w - 8;
                    if (uvIndex >= 0 && (size_t)uvIndex < targetUv.size() && (size_t)uvIndex < p.morphedUv.size()) {
                        p.morphedUv[uvIndex] += targetUv[uvIndex] * weight;
                    }
                }
            }
            const size_t count = p.morphedPosition.size() / 3;
            float* out = &mVertices[(size_t)slot.offset * 5];
            for (size_t v = 0; v < count; v++) {
                TransformPoint(node.global, &p.morphedPosition[v * 3], out);
                out[3] = p.morphedUv[v * 2];
                out[4] = p.morphedUv[v * 2 + 1];
                out += 5;
            }
            low = std::min(low, (size_t)slot.offset * 5);
            high = std::max(high, ((size_t)slot.offset + count) * 5);
        }
        node.vertexDirty = false;
        node.geometryDirty = false;
    }
    mVerticesDirty = false;
    if (high <= low) {
        return;
    }
    GLint arrayBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBufferSubData(GL_ARRAY_BUFFER, low * sizeof(float), (high - low) * sizeof(float), &mVertices[low]);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)arrayBuffer);
}

void XrVirtualKeyboard::ModelMatrix(float out[16]) const {
    const float t[3] = { mPose.position.x, mPose.position.y, mPose.position.z };
    const float q[4] = { mPose.orientation.x, mPose.orientation.y, mPose.orientation.z, mPose.orientation.w };
    const float s[3] = { mScale, mScale, mScale };
    FromTrs(t, q, s, out);
}

bool XrVirtualKeyboard::RayHit(const XrPosef& aim, XrVector3f* point) const {
    if (!mShown || !mPoseValid || mCollisionNode < 0) {
        return false;
    }
    float model[16];
    float box[16];
    float inverse[16];
    ModelMatrix(model);
    Multiply(model, mNodes[mCollisionNode].global, box);
    if (!InvertAffine(box, inverse)) {
        return false;
    }
    const float from[3] = { aim.position.x, aim.position.y, aim.position.z };
    const float forward[3] = { 0.0f, 0.0f, -1.0f };
    float along[3];
    Rotate(aim.orientation, forward, along);
    float o[3];
    float d[3];
    TransformPoint(inverse, from, o);
    TransformVector(inverse, along, d);

    float near = 0.0f;
    float far = 1e30f;
    for (int k = 0; k < 3; k++) {
        const float span = mCollisionMax[k] - mCollisionMin[k];
        const float margin = std::max(span * HIT_MARGIN, 1e-3f);
        const float low = mCollisionMin[k] - margin;
        const float high = mCollisionMax[k] + margin;
        if (fabsf(d[k]) < 1e-9f) {
            if (o[k] < low || o[k] > high) {
                return false;
            }
            continue;
        }
        float t0 = (low - o[k]) / d[k];
        float t1 = (high - o[k]) / d[k];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        near = std::max(near, t0);
        far = std::min(far, t1);
        if (near > far) {
            return false;
        }
    }
    const float local[3] = { o[0] + d[0] * near, o[1] + d[1] * near, o[2] + d[2] * near };
    float world[3];
    TransformPoint(box, local, world);
    *point = { world[0], world[1], world[2] };
    return true;
}

bool XrVirtualKeyboard::KeyboardPoint(float x, float y, XrVector3f* point, XrVector3f* normal) const {
    if (!mShown || !mPoseValid || mCollisionNode < 0) {
        return false;
    }
    float model[16];
    float box[16];
    ModelMatrix(model);
    Multiply(model, mNodes[mCollisionNode].global, box);
    const float local[3] = { mCollisionMin[0] + (mCollisionMax[0] - mCollisionMin[0]) * x,
                             mCollisionMax[1] - (mCollisionMax[1] - mCollisionMin[1]) * y, mCollisionMax[2] };
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    float world[3];
    float facing[3];
    TransformPoint(box, local, world);
    TransformVector(box, up, facing);
    const float length = sqrtf(facing[0] * facing[0] + facing[1] * facing[1] + facing[2] * facing[2]);
    if (length <= 1e-9f) {
        return false;
    }
    *point = { world[0], world[1], world[2] };
    *normal = { facing[0] / length, facing[1] / length, facing[2] / length };
    return true;
}

void XrVirtualKeyboard::SendRay(int hand, bool handTracked, const XrPosef& aim, bool pressed) {
    if (!mShown) {
        return;
    }
    XrVirtualKeyboardInputInfoMETA info{ XR_TYPE_VIRTUAL_KEYBOARD_INPUT_INFO_META };
    if (handTracked) {
        info.inputSource = hand == 0 ? XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_HAND_RAY_LEFT_META
                                     : XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_HAND_RAY_RIGHT_META;
    } else {
        info.inputSource = hand == 0 ? XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_LEFT_META
                                     : XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_RIGHT_META;
    }
    info.inputSpace = mSpace;
    info.inputPoseInSpace = aim;
    info.inputState = pressed ? XR_VIRTUAL_KEYBOARD_INPUT_STATE_PRESSED_BIT_META : 0;
    XrPosef root = aim;
    mSendInput(mKeyboard, &info, &root);
}

void XrVirtualKeyboard::Draw(const float viewProjection[16]) {
    if (!mShown || !mPoseValid || mBatches.empty()) {
        return;
    }
    float model[16];
    float mvp[16];
    ModelMatrix(model);
    Multiply(viewProjection, model, mvp);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(mProgram);
    glUniformMatrix4fv(mMvpLoc, 1, GL_FALSE, mvp);
    glBindVertexArray(mVao);
    glActiveTexture(GL_TEXTURE0);
    for (const Batch& batch : mBatches) {
        if (batch.count == 0) {
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, batch.texture >= 0 ? mTextures[batch.texture].gl : mWhite);
        glUniform4fv(mBaseColorLoc, 1, batch.baseColor);
        glDrawElements(GL_TRIANGLES, (GLsizei)batch.count, GL_UNSIGNED_INT,
                       (const void*)((uintptr_t)batch.first * sizeof(uint32_t)));
    }
    glDisable(GL_DEPTH_TEST);
}

std::string XrVirtualKeyboard::TakeText() {
    std::string text;
    text.swap(mText);
    return text;
}

int XrVirtualKeyboard::TakeBackspaces() {
    const int count = mBackspaces;
    mBackspaces = 0;
    return count;
}

bool XrVirtualKeyboard::TakeEnter() {
    const bool enter = mEnter;
    mEnter = false;
    return enter;
}

bool XrVirtualKeyboard::TakeClosed() {
    const bool closed = mClosed;
    mClosed = false;
    return closed;
}

} // namespace Fast

#endif
