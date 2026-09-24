#ifdef ENABLE_XR_WINDOW

#include "fast/backends/gfx_stereo_replay.h"

#include <cstring>

namespace Fast {

GfxStereoReplay::GfxStereoReplay(GfxRenderingAPI* inner) : mInner(inner) {
}

GfxStereoReplay::~GfxStereoReplay() {
    delete mInner;
}

void GfxStereoReplay::BeginRecord(const std::unordered_map<int, int>* fbTwins) {
    mCmds.clear();
    mVbo.clear();
    mFbTwins = fbTwins;
    mRecording = true;
    mReplayable = true;
    RecordState();
}

void GfxStereoReplay::EndRecord() {
    mRecording = false;
}

bool GfxStereoReplay::IsRecording() const {
    return mRecording;
}

bool GfxStereoReplay::IsReplayable() const {
    return mReplayable && !mRecording;
}

GfxStereoReplay::Cmd& GfxStereoReplay::Push(Op op) {
    mCmds.emplace_back();
    Cmd& cmd = mCmds.back();
    cmd.op = op;
    return cmd;
}

int GfxStereoReplay::Twin(int fbId) const {
    if (mFbTwins != nullptr) {
        auto it = mFbTwins->find(fbId);
        if (it != mFbTwins->end()) {
            return it->second;
        }
    }
    return fbId;
}

void GfxStereoReplay::RecordState() {
    if (mShader != nullptr) {
        Push(Op::LoadShader).prg = mShader;
    }
    for (int tile = 0; tile < TRACKED_TILES; tile++) {
        const TrackedTile& t = mTiles[tile];
        if (t.valid) {
            Cmd& cmd = Push(t.isFb ? Op::SelectTextureFb : Op::SelectTexture);
            cmd.a[0] = tile;
            cmd.a[1] = t.id;
        }
        if (t.samplerValid) {
            Cmd& cmd = Push(Op::SamplerParameters);
            cmd.a[0] = tile;
            cmd.a[1] = t.linear;
            cmd.a[2] = (int32_t)t.cms;
            cmd.a[3] = (int32_t)t.cmt;
        }
    }
    if (mDepthValid) {
        Cmd& cmd = Push(Op::DepthTestAndMask);
        cmd.a[0] = mDepthTest;
        cmd.a[1] = mDepthMask;
    }
    if (mDecalValid) {
        Push(Op::ZmodeDecal).a[0] = mDecal;
    }
    if (mAlphaValid) {
        Push(Op::UseAlpha).a[0] = mAlpha;
    }
}

void GfxStereoReplay::DrawStereoTriangles(float* left, const float* right, size_t len, size_t tris) {
    mInner->DrawTriangles(left, len, tris);
    if (mRecording) {
        Cmd& cmd = Push(Op::Triangles);
        cmd.off = mVbo.size();
        cmd.len = len;
        cmd.tris = tris;
        mVbo.insert(mVbo.end(), right, right + len);
    }
}

uint32_t GfxStereoReplay::Replay() {
    uint32_t draws = 0;
    for (const Cmd& cmd : mCmds) {
        switch (cmd.op) {
            case Op::UnloadShader:
                mInner->UnloadShader(cmd.prg);
                break;
            case Op::LoadShader:
                mInner->LoadShader(cmd.prg);
                break;
            case Op::SelectTexture:
                mInner->SelectTexture(cmd.a[0], (uint32_t)cmd.a[1]);
                break;
            case Op::SelectTextureFb:
                mInner->SelectTextureFb(Twin(cmd.a[1]));
                break;
            case Op::SamplerParameters:
                mInner->SetSamplerParameters(cmd.a[0], cmd.a[1] != 0, (uint32_t)cmd.a[2], (uint32_t)cmd.a[3]);
                break;
            case Op::DepthTestAndMask:
                mInner->SetDepthTestAndMask(cmd.a[0] != 0, cmd.a[1] != 0);
                break;
            case Op::ZmodeDecal:
                mInner->SetZmodeDecal(cmd.a[0] != 0);
                break;
            case Op::Viewport:
                mInner->SetViewport(cmd.a[0], cmd.a[1], cmd.a[2], cmd.a[3]);
                break;
            case Op::Scissor:
                mInner->SetScissor(cmd.a[0], cmd.a[1], cmd.a[2], cmd.a[3]);
                break;
            case Op::UseAlpha:
                mInner->SetUseAlpha(cmd.a[0] != 0);
                break;
            case Op::Triangles:
                mInner->DrawTriangles(mVbo.data() + cmd.off, cmd.len, cmd.tris);
                draws++;
                break;
            case Op::FramebufferParameters:
                mInner->UpdateFramebufferParameters(Twin(cmd.a[0]), (uint32_t)cmd.a[1], (uint32_t)cmd.a[2],
                                                    (uint32_t)cmd.a[3], cmd.a[4] != 0, cmd.a[5] != 0, cmd.a[6] != 0,
                                                    cmd.a[7] != 0);
                break;
            case Op::DrawToFramebuffer:
                mInner->StartDrawToFramebuffer(Twin(cmd.a[0]), cmd.f);
                break;
            case Op::CopyFramebuffer:
                mInner->CopyFramebuffer(Twin(cmd.a[0]), Twin(cmd.a[1]), cmd.a[2], cmd.a[3], cmd.a[4], cmd.a[5],
                                        cmd.a[6], cmd.a[7], cmd.a[8], cmd.a[9]);
                break;
            case Op::ClearFramebuffer:
                mInner->ClearFramebuffer(cmd.a[0] != 0, cmd.a[1] != 0);
                break;
            case Op::ClearDepthRegion:
                mInner->ClearDepthRegion(cmd.a[0], cmd.a[1], cmd.a[2], cmd.a[3]);
                break;
            case Op::ResolveMsaa:
                mInner->ResolveMSAAColorBuffer(Twin(cmd.a[0]), Twin(cmd.a[1]));
                break;
            case Op::PrimDepth:
                mInner->SetCurrentPrimDepth(cmd.f);
                break;
        }
    }
    return draws;
}

const char* GfxStereoReplay::GetName() {
    return mInner->GetName();
}

int GfxStereoReplay::GetMaxTextureSize() {
    return mInner->GetMaxTextureSize();
}

GfxClipParameters GfxStereoReplay::GetClipParameters() {
    return mInner->GetClipParameters();
}

void GfxStereoReplay::UnloadShader(ShaderProgram* oldPrg) {
    mInner->UnloadShader(oldPrg);
    if (mRecording) {
        Push(Op::UnloadShader).prg = oldPrg;
    }
}

void GfxStereoReplay::LoadShader(ShaderProgram* newPrg) {
    mInner->LoadShader(newPrg);
    mShader = newPrg;
    if (mRecording) {
        Push(Op::LoadShader).prg = newPrg;
    }
}

void GfxStereoReplay::ClearShaderCache() {
    mInner->ClearShaderCache();
    mShader = nullptr;
    mReplayable = false;
}

ShaderProgram* GfxStereoReplay::CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) {
    ShaderProgram* prg = mInner->CreateAndLoadNewShader(shaderId0, shaderId1);
    mShader = prg;
    if (mRecording) {
        Push(Op::LoadShader).prg = prg;
    }
    return prg;
}

ShaderProgram* GfxStereoReplay::LookupShader(uint64_t shaderId0, uint64_t shaderId1) {
    return mInner->LookupShader(shaderId0, shaderId1);
}

void GfxStereoReplay::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    mInner->ShaderGetInfo(prg, numInputs, usedTextures);
}

uint32_t GfxStereoReplay::NewTexture() {
    return mInner->NewTexture();
}

void GfxStereoReplay::SelectTexture(int tile, uint32_t textureId) {
    mInner->SelectTexture(tile, textureId);
    if (tile >= 0 && tile < TRACKED_TILES) {
        mTiles[tile].valid = true;
        mTiles[tile].isFb = false;
        mTiles[tile].id = (int)textureId;
    }
    if (mRecording) {
        Cmd& cmd = Push(Op::SelectTexture);
        cmd.a[0] = tile;
        cmd.a[1] = (int32_t)textureId;
    }
}

void GfxStereoReplay::UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) {
    mInner->UploadTexture(rgba32Buf, width, height);
}

void GfxStereoReplay::SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) {
    mInner->SetSamplerParameters(sampler, linearFilter, cms, cmt);
    if (sampler >= 0 && sampler < TRACKED_TILES) {
        mTiles[sampler].samplerValid = true;
        mTiles[sampler].linear = linearFilter;
        mTiles[sampler].cms = cms;
        mTiles[sampler].cmt = cmt;
    }
    if (mRecording) {
        Cmd& cmd = Push(Op::SamplerParameters);
        cmd.a[0] = sampler;
        cmd.a[1] = linearFilter;
        cmd.a[2] = (int32_t)cms;
        cmd.a[3] = (int32_t)cmt;
    }
}

void GfxStereoReplay::SetDepthTestAndMask(bool depthTest, bool zUpd) {
    mInner->SetDepthTestAndMask(depthTest, zUpd);
    mDepthValid = true;
    mDepthTest = depthTest;
    mDepthMask = zUpd;
    if (mRecording) {
        Cmd& cmd = Push(Op::DepthTestAndMask);
        cmd.a[0] = depthTest;
        cmd.a[1] = zUpd;
    }
}

void GfxStereoReplay::SetZmodeDecal(bool decal) {
    mInner->SetZmodeDecal(decal);
    mDecalValid = true;
    mDecal = decal;
    if (mRecording) {
        Push(Op::ZmodeDecal).a[0] = decal;
    }
}

void GfxStereoReplay::SetViewport(int x, int y, int width, int height) {
    mInner->SetViewport(x, y, width, height);
    if (mRecording) {
        Cmd& cmd = Push(Op::Viewport);
        cmd.a[0] = x;
        cmd.a[1] = y;
        cmd.a[2] = width;
        cmd.a[3] = height;
    }
}

void GfxStereoReplay::SetScissor(int x, int y, int width, int height) {
    mInner->SetScissor(x, y, width, height);
    if (mRecording) {
        Cmd& cmd = Push(Op::Scissor);
        cmd.a[0] = x;
        cmd.a[1] = y;
        cmd.a[2] = width;
        cmd.a[3] = height;
    }
}

void GfxStereoReplay::SetUseAlpha(bool useAlpha) {
    mInner->SetUseAlpha(useAlpha);
    mAlphaValid = true;
    mAlpha = useAlpha;
    if (mRecording) {
        Push(Op::UseAlpha).a[0] = useAlpha;
    }
}

void GfxStereoReplay::DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) {
    DrawStereoTriangles(bufVbo, bufVbo, bufVboLen, bufVboNumTris);
}

void GfxStereoReplay::Init() {
    mInner->Init();
}

void GfxStereoReplay::OnResize() {
    mInner->OnResize();
}

void GfxStereoReplay::StartFrame() {
    mInner->StartFrame();
}

void GfxStereoReplay::HoldFrameNoise(bool hold) {
    mInner->HoldFrameNoise(hold);
}

void GfxStereoReplay::EndFrame() {
    mInner->EndFrame();
}

void GfxStereoReplay::FinishRender() {
    mInner->FinishRender();
}

int GfxStereoReplay::CreateFramebuffer() {
    return mInner->CreateFramebuffer();
}

void GfxStereoReplay::UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height, uint32_t msaaLevel,
                                                  bool openglInvertY, bool renderTarget, bool hasDepthBuffer,
                                                  bool canExtractDepth) {
    mInner->UpdateFramebufferParameters(fbId, width, height, msaaLevel, openglInvertY, renderTarget, hasDepthBuffer,
                                        canExtractDepth);
    if (mRecording) {
        Cmd& cmd = Push(Op::FramebufferParameters);
        cmd.a[0] = fbId;
        cmd.a[1] = (int32_t)width;
        cmd.a[2] = (int32_t)height;
        cmd.a[3] = (int32_t)msaaLevel;
        cmd.a[4] = openglInvertY;
        cmd.a[5] = renderTarget;
        cmd.a[6] = hasDepthBuffer;
        cmd.a[7] = canExtractDepth;
    }
}

void GfxStereoReplay::StartDrawToFramebuffer(int fbId, float noiseScale) {
    mInner->StartDrawToFramebuffer(fbId, noiseScale);
    if (mRecording) {
        Cmd& cmd = Push(Op::DrawToFramebuffer);
        cmd.a[0] = fbId;
        cmd.f = noiseScale;
    }
}

void GfxStereoReplay::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0,
                                      int dstY0, int dstX1, int dstY1) {
    mInner->CopyFramebuffer(fbDstId, fbSrcId, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1);
    if (mRecording) {
        Cmd& cmd = Push(Op::CopyFramebuffer);
        const int32_t args[10] = { fbDstId, fbSrcId, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1 };
        memcpy(cmd.a, args, sizeof(args));
    }
}

void GfxStereoReplay::ClearFramebuffer(bool color, bool depth) {
    mInner->ClearFramebuffer(color, depth);
    if (mRecording) {
        Cmd& cmd = Push(Op::ClearFramebuffer);
        cmd.a[0] = color;
        cmd.a[1] = depth;
    }
}

void GfxStereoReplay::ClearDepthRegion(int x, int y, int w, int h) {
    mInner->ClearDepthRegion(x, y, w, h);
    if (mRecording) {
        Cmd& cmd = Push(Op::ClearDepthRegion);
        cmd.a[0] = x;
        cmd.a[1] = y;
        cmd.a[2] = w;
        cmd.a[3] = h;
    }
}

void GfxStereoReplay::ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    mInner->ReadFramebufferToCPU(fbId, width, height, rgba16Buf);
}

void GfxStereoReplay::ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) {
    mInner->ResolveMSAAColorBuffer(fbIdTarget, fbIdSrc);
    if (mRecording) {
        Cmd& cmd = Push(Op::ResolveMsaa);
        cmd.a[0] = fbIdTarget;
        cmd.a[1] = fbIdSrc;
    }
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxStereoReplay::GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) {
    return mInner->GetPixelDepth(fbId, coordinates);
}

void* GfxStereoReplay::GetFramebufferTextureId(int fbId) {
    return mInner->GetFramebufferTextureId(fbId);
}

void GfxStereoReplay::SelectTextureFb(int fbId) {
    mInner->SelectTextureFb(fbId);
    mTiles[0].valid = true;
    mTiles[0].isFb = true;
    mTiles[0].id = fbId;
    if (mRecording) {
        Cmd& cmd = Push(Op::SelectTextureFb);
        cmd.a[0] = 0;
        cmd.a[1] = fbId;
    }
}

void GfxStereoReplay::DeleteTexture(uint32_t texId) {
    mInner->DeleteTexture(texId);
    for (TrackedTile& tile : mTiles) {
        if (tile.valid && !tile.isFb && tile.id == (int)texId) {
            tile.valid = false;
        }
    }
    if (mRecording) {
        mReplayable = false;
    }
}

void GfxStereoReplay::SetTextureFilter(FilteringMode mode) {
    mInner->SetTextureFilter(mode);
}

FilteringMode GfxStereoReplay::GetTextureFilter() {
    return mInner->GetTextureFilter();
}

void GfxStereoReplay::SetSrgbMode() {
    mInner->SetSrgbMode();
}

ImTextureID GfxStereoReplay::GetTextureById(int id) {
    return mInner->GetTextureById(id);
}

void GfxStereoReplay::SetCurrentPrimDepth(float depth) {
    mInner->SetCurrentPrimDepth(depth);
    if (mRecording) {
        Push(Op::PrimDepth).f = depth;
    }
}

} // namespace Fast

#endif
