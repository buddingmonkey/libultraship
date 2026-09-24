#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "gfx_rendering_api.h"

namespace Fast {

class GfxStereoReplay final : public GfxRenderingAPI {
  public:
    explicit GfxStereoReplay(GfxRenderingAPI* inner);
    ~GfxStereoReplay() override;

    void BeginRecord(const std::unordered_map<int, int>* fbTwins);
    void EndRecord();
    bool IsRecording() const;
    bool IsReplayable() const;
    void DrawStereoTriangles(float* left, const float* right, size_t len, size_t tris);
    uint32_t Replay();

    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    void ClearShaderCache() override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depthTest, bool zUpd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void HoldFrameNoise(bool hold) override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height, uint32_t msaaLevel, bool openglInvertY,
                                     bool renderTarget, bool hasDepthBuffer, bool canExtractDepth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ClearDepthRegion(int x, int y, int w, int h) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;
    void SetCurrentPrimDepth(float depth) override;

  private:
    static constexpr int TRACKED_TILES = 8;

    enum class Op : uint8_t {
        UnloadShader,
        LoadShader,
        SelectTexture,
        SelectTextureFb,
        SamplerParameters,
        DepthTestAndMask,
        ZmodeDecal,
        Viewport,
        Scissor,
        UseAlpha,
        Triangles,
        FramebufferParameters,
        DrawToFramebuffer,
        CopyFramebuffer,
        ClearFramebuffer,
        ClearDepthRegion,
        ResolveMsaa,
        PrimDepth,
    };

    struct Cmd {
        Op op;
        int32_t a[10];
        float f;
        ShaderProgram* prg;
        size_t off;
        size_t len;
        size_t tris;
    };

    struct TrackedTile {
        bool valid = false;
        bool isFb = false;
        int id = 0;
        bool samplerValid = false;
        bool linear = false;
        uint32_t cms = 0;
        uint32_t cmt = 0;
    };

    Cmd& Push(Op op);
    int Twin(int fbId) const;
    void RecordState();

    GfxRenderingAPI* mInner;
    bool mRecording = false;
    bool mReplayable = false;
    std::vector<Cmd> mCmds;
    std::vector<float> mVbo;
    const std::unordered_map<int, int>* mFbTwins = nullptr;

    TrackedTile mTiles[TRACKED_TILES];
    bool mDepthValid = false;
    bool mDepthTest = false;
    bool mDepthMask = false;
    bool mDecalValid = false;
    bool mDecal = false;
    bool mAlphaValid = false;
    bool mAlpha = false;
    ShaderProgram* mShader = nullptr;
};

} // namespace Fast
