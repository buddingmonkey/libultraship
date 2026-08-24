#include "fast/Fast3dGui.h"

#include <chrono>

#include <spdlog/spdlog.h>
#include <imgui_internal.h>

#include "fast/Fast3dWindow.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "fast/backends/gfx_metal.h"
#include "fast/backends/gfx_visionos.h"
#include "fast/interpreter.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/resource/type/Texture.h"
#include "ship/window/gui/resource/GuiTextureFactory.h"
#include "ship/resource/File.h"

#ifdef __APPLE__
#include <SDL_hints.h>
#include <SDL_video.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_sdl2.h>
#else
#include <SDL2/SDL_hints.h>
#include <SDL2/SDL_video.h>
#endif

#if defined(__ANDROID__) || defined(__IOS__)
#include "ship/port/mobile/MobileImpl.h"
#endif

#ifdef ENABLE_OPENGL
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#endif

#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

// NOLINTNEXTLINE
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

namespace Fast {

Fast3dGui::Fast3dGui() : Ship::Gui() {
}

Fast3dGui::Fast3dGui(std::vector<std::shared_ptr<Ship::GuiWindow>> guiWindows) : Ship::Gui(guiWindows) {
}

void Fast3dGui::Init(GuiWindowInitData windowImpl) {
    mImpl = windowImpl;
    Gui::Init();
}

bool Fast3dGui::SupportsViewports() {
#ifdef __linux__
    const char* currentDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (currentDesktop && std::string(currentDesktop) == "gamescope") {
        return false;
    }
#endif

#if defined(__ANDROID__) || defined(__IOS__)
    return false;
#endif

    return true;
}

void Fast3dGui::HandleWindowEvents(Fast::WindowEvent event) {
    switch (mImpl.Backend) {
        case WindowBackend::FAST3D_SDL_OPENGL:
        case WindowBackend::FAST3D_SDL_METAL:
            ImGui_ImplSDL2_ProcessEvent(static_cast<const SDL_Event*>(event.Sdl.Event));
#if defined(__ANDROID__) || defined(__IOS__)
            Ship::Mobile::ImGuiProcessEvent(ImGui::GetIO().WantTextInput);
#endif
            break;
#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(event.Win32.Handle), event.Win32.Msg, event.Win32.Param1,
                                           event.Win32.Param2);
            break;
#endif
        default:
            break;
    }
}

#ifdef __VISIONOS__
// There is no platform backend to give ImGui a frame time, and ImGui needs one above zero.
static float VisionOSDeltaTime() {
    static std::chrono::steady_clock::time_point sLast{};
    const auto now = std::chrono::steady_clock::now();
    const float fallback = 1.0f / 90.0f;
    if (sLast.time_since_epoch().count() == 0) {
        sLast = now;
        return fallback;
    }
    const float delta = std::chrono::duration<float>(now - sLast).count();
    sLast = now;
    return delta > 0.0f ? delta : fallback;
}
#endif

void Fast3dGui::ImGuiWMInit() {
    switch (mImpl.Backend) {
        case WindowBackend::FAST3D_SDL_OPENGL:
            SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
            if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_ALLOW_BACKGROUND_INPUTS, 1)) {
                SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            }
            ImGui_ImplSDL2_InitForOpenGL(static_cast<SDL_Window*>(mImpl.Opengl.Window), mImpl.Opengl.Context);
            break;
#if __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
            SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
            if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_ALLOW_BACKGROUND_INPUTS, 1)) {
                SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            }
            ImGui_ImplSDL2_InitForMetal(static_cast<SDL_Window*>(mImpl.Metal.Window));
            break;
#endif
#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplWin32_Init(mImpl.Dx11.Window);
            break;
#endif
        default:
            break;
    }

    // Initial gamepad bind once the SDL2 backend exists (no-op for DX/Metal).
    RefreshImGuiGamepads();
}

void Fast3dGui::ImGuiWMShutdown() {
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            ImGui_ImplSDL2_Shutdown();
            break;
#endif
#if __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
            ImGui_ImplSDL2_Shutdown();
            break;
#endif
#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplWin32_Shutdown();
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiBackendInit() {
    auto window = Ship::Context::GetRawInstance()->GetWindow();
    mInterpreter = std::dynamic_pointer_cast<Fast3dWindow>(window)->GetInterpreterWeak();
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
#ifdef __APPLE__
            ImGui_ImplOpenGL3_Init("#version 410 core");
#elif USE_OPENGLES
            ImGui_ImplOpenGL3_Init("#version 300 es");
#else
            ImGui_ImplOpenGL3_Init("#version 120");
#endif
            break;
#endif

#ifdef __APPLE__
        case WindowBackend::FAST3D_SDL_METAL: {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->MetalInit(mImpl.Metal.Renderer);
            break;
        }
#endif
#ifdef __VISIONOS__
        case WindowBackend::FAST3D_VISIONOS_METAL: {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->MetalInitImGui();
            break;
        }
#endif

#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplDX11_Init(static_cast<ID3D11Device*>(mImpl.Dx11.Device),
                                static_cast<ID3D11DeviceContext*>(mImpl.Dx11.DeviceContext));
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiBackendShutdown() {
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            ImGui_ImplOpenGL3_Shutdown();
            break;
#endif
#if __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
#ifdef __VISIONOS__
        case WindowBackend::FAST3D_VISIONOS_METAL:
#endif
            ImGui_ImplMetal_Shutdown();
            break;
#endif
#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplDX11_Shutdown();
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiBackendNewFrame() {
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            ImGui_ImplOpenGL3_NewFrame();
            break;
#endif

#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplDX11_NewFrame();
            break;
#endif

#ifdef __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
#ifdef __VISIONOS__
        case WindowBackend::FAST3D_VISIONOS_METAL:
#endif
        {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->NewFrame();
            break;
        }
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiWMNewFrame() {
    switch (mImpl.Backend) {
        case WindowBackend::FAST3D_SDL_OPENGL: {
            ImGui_ImplSDL2_NewFrame();
#ifdef ENABLE_OPENXR
            // ImGui's SDL2 backend takes the display size from the window. A headset window is a
            // rectangle in the room, and the backend draws the eye it is on at the size of that
            // rectangle, not at the size of the panel SDL holds. Take the size the window backend
            // reports so the menu is laid out and drawn in the same units the picture is.
            auto interpreter = mInterpreter.lock();
            if (interpreter != nullptr) {
                uint32_t width = 0;
                uint32_t height = 0;
                int32_t posX = 0;
                int32_t posY = 0;
                interpreter->GetDimensions(&width, &height, &posX, &posY);
                if (width > 0 && height > 0) {
                    ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
                    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                }
            }
#endif
            break;
        }
        case WindowBackend::FAST3D_SDL_METAL: {
            ImGui_ImplSDL2_NewFrame();

            // ImGui's SDL2 backend derives DisplayFramebufferScale from SDL_GL_GetDrawableSize().
            // UIKit only implements that for GL views -- given a Metal view it falls through to
            // SDL_GetWindowSize(), which reports points, so the scale comes out 1.0 on a 2x/3x iOS
            // display instead of 2.0/3.0. GfxRenderingAPIMetal::RenderDrawData() then finds
            // DisplaySize * scale disagreeing with its pixel-sized screen texture and drops every
            // ImGui frame, leaving nothing but the cleared framebuffer on screen.
            SDL_Window* metalWindow = static_cast<SDL_Window*>(mImpl.Metal.Window);
            int pixelWidth = 0, pixelHeight = 0, pointWidth = 0, pointHeight = 0;
            SDL_Metal_GetDrawableSize(metalWindow, &pixelWidth, &pixelHeight);
            SDL_GetWindowSize(metalWindow, &pointWidth, &pointHeight);
            if (pixelWidth > 0 && pixelHeight > 0 && pointWidth > 0 && pointHeight > 0) {
                ImGui::GetIO().DisplayFramebufferScale =
                    ImVec2(static_cast<float>(pixelWidth) / pointWidth, static_cast<float>(pixelHeight) / pointHeight);
            }
            break;
        }
#ifdef __VISIONOS__
        case WindowBackend::FAST3D_VISIONOS_METAL: {
            // No SDL window means no platform backend, so nothing else fills these in. The size
            // must equal the game texture, or RenderDrawData drops every ImGui frame.
            auto interpreter = mInterpreter.lock();
            uint32_t width = mImpl.VisionOS.Width;
            uint32_t height = mImpl.VisionOS.Height;
            if (interpreter != nullptr) {
                int32_t posX = 0;
                int32_t posY = 0;
                interpreter->GetDimensions(&width, &height, &posX, &posY);
            }
            if (width > 0 && height > 0) {
                ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
            }
            ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            ImGui::GetIO().DeltaTime = VisionOSDeltaTime();

            // Take the pointer before the list is cleared, so the tracking area the system aimed at
            // can still be looked up among this frame's items.
            {
                static float sX = -FLT_MAX;
                static float sY = -FLT_MAX;
                static bool sPressed = false;
                VisionOSPointer next{};
                if (PeekVisionOSPointer(&next)) {
                    float wantX = next.X;
                    float wantY = next.Y;
                    bool wantValid = next.Valid;
                    // The system already decided which item it aimed at, and it drew the highlight
                    // for that item. Take its answer, so the press cannot land somewhere else.
                    if (next.Identifier != 0) {
                        for (size_t i = 0; i < GetVisionOSTrackingRectCount(); ++i) {
                            const VisionOSTrackingRect rect = GetVisionOSTrackingRect(i);
                            if (rect.Identifier == next.Identifier) {
                                wantX = (rect.MinX + rect.MaxX) * 0.5f;
                                wantY = (rect.MinY + rect.MaxY) * 0.5f;
                                wantValid = true;
                                break;
                            }
                        }
                    }

                    if (wantValid && (wantX != sX || wantY != sY)) {
                        sX = wantX;
                        sY = wantY;
                        ImGui::GetIO().AddMousePosEvent(sX, sY);
                    } else {
                        const bool wanted = wantValid && next.Pressed;
                        if (wanted != sPressed) {
                            sPressed = wanted;
                            ImGuiContext* context = ImGui::GetCurrentContext();
                            SPDLOG_INFO("visionOS: pointer {} at {:.0f},{:.0f} area {}; hovered {} '{}'",
                                        sPressed ? "down" : "up", sX, sY, next.Identifier, context->HoveredId,
                                        GetVisionOSItemLabel(context->HoveredId));
                        }
                        PopVisionOSPointer();
                    }
                }
                ImGui::GetIO().AddMouseButtonEvent(0, sPressed);
            }

            {
                static size_t sReported = SIZE_MAX;
                const size_t collected = GetVisionOSTrackingRectCount();
                if (collected != sReported) {
                    sReported = collected;
                    SPDLOG_INFO("visionOS: {} ImGui items offered as tracking areas", collected);
                }
            }
            BeginVisionOSTrackingRects();

            break;
        }
#endif
#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplWin32_NewFrame();
            break;
#endif
        default:
            break;
    }
}

// Bind ImGui's SDL2 gamepad backend to the controller(s) the
// ControlDeck has already opened
void Fast3dGui::RefreshImGuiGamepads() {
    if (mImpl.Backend != WindowBackend::FAST3D_SDL_OPENGL && mImpl.Backend != WindowBackend::FAST3D_SDL_METAL) {
        return;
    }

    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll, nullptr, 0);
}

void Fast3dGui::ImGuiRenderDrawData(ImDrawData* data) {
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            ImGui_ImplOpenGL3_RenderDrawData(data);
            break;
#endif

#ifdef __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
#ifdef __VISIONOS__
        case WindowBackend::FAST3D_VISIONOS_METAL:
#endif
        {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->RenderDrawData(data);
            break;
        }
#endif

#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplDX11_RenderDrawData(data);
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::DrawFloatingWindows() {
    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
        return;
    }

    // OpenGL requires extra platform handling for the GL context
    if (mImpl.Backend == WindowBackend::FAST3D_SDL_OPENGL && mImpl.Opengl.Context != nullptr) {
        // Backup window and context before calling RenderPlatformWindowsDefault
        SDL_Window* backupCurrentWindow = SDL_GL_GetCurrentWindow();
        SDL_GLContext backupCurrentContext = SDL_GL_GetCurrentContext();

        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        // Restore GL context for next frame
        SDL_GL_MakeCurrent(backupCurrentWindow, backupCurrentContext);
    } else {
#ifdef __APPLE__
        // Metal requires additional frame setup to get ImGui ready for drawing floating windows
        if (mImpl.Backend == WindowBackend::FAST3D_SDL_METAL) {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->SetupFloatingFrame();
        }
#endif

        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void Fast3dGui::CalculateGameViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;

    ImGui::Begin("Main Game", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    ImVec2 mainPos = ImGui::GetWindowPos();
    mainPos.x -= mTemporaryWindowPos.x;
    mainPos.y -= mTemporaryWindowPos.y;
    ImVec2 size = ImGui::GetContentRegionAvail();
    const auto interpreter = mInterpreter.lock().get();
    interpreter->mCurDimensions.width = (uint32_t)(size.x * mInterpreter.lock()->mCurDimensions.internal_mul);
    interpreter->mCurDimensions.height = (uint32_t)(size.y * mInterpreter.lock()->mCurDimensions.internal_mul);
    interpreter->mGameWindowViewport.x = (int16_t)mainPos.x;
    interpreter->mGameWindowViewport.y = (int16_t)mainPos.y;
    interpreter->mGameWindowViewport.width = (int16_t)size.x;
    interpreter->mGameWindowViewport.height = (int16_t)size.y;

    if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled",
                                                                           0)) {
        ApplyResolutionChanges();
    }

    switch (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_LOW_RES_MODE, 0)) {
        case 1: { // N64 Mode
            interpreter->mCurDimensions.width = 320;
            interpreter->mCurDimensions.height = 240;
            /*
            const int sw = size.y * 320 / 240;
            mInterpreter.lock()->mGameWindowViewport.x += ((int)size.x - sw) / 2;
            mInterpreter.lock()->mGameWindowViewport.width = sw;*/
            break;
        }
        case 2: { // 240p Widescreen
            constexpr int vertRes = 240;
            interpreter->mCurDimensions.width = vertRes * size.x / size.y;
            interpreter->mCurDimensions.height = vertRes;
            break;
        }
        case 3: { // 480p Widescreen
            constexpr int vertRes = 480;
            interpreter->mCurDimensions.width = vertRes * size.x / size.y;
            interpreter->mCurDimensions.height = vertRes;
            break;
        }
    }

    ImGui::End();
}

void Fast3dGui::DrawGame() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;

    ImGui::Begin("Main Game", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    GetGameOverlay()->Draw();

    ImVec2 mainPos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImVec2 pos = ImVec2(0, 0);
    const auto interpreter = mInterpreter.lock().get();

    if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_LOW_RES_MODE, 0) ==
        1) { // N64 Mode takes priority
        const float sw = size.y * 320.0f / 240.0f;
        pos = ImVec2(floor(size.x / 2 - sw / 2), 0);
        size = ImVec2(sw, size.y);
    } else if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
                   CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled", 0)) {
        if (!Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
                CVAR_PREFIX_ADVANCED_RESOLUTION ".PixelPerfectMode", 0)) {
            if (!Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
                    CVAR_PREFIX_ADVANCED_RESOLUTION ".IgnoreAspectCorrection", 0)) {
                float sWdth = size.y * interpreter->mCurDimensions.width / interpreter->mCurDimensions.height;
                float sHght = size.x * interpreter->mCurDimensions.height / interpreter->mCurDimensions.width;

                float sPosX = floor(size.x / 2.0f - sWdth / 2.0f);
                float sPosY = floor(size.y / 2.0f - sHght / 2.0f);
                if (sPosY < 0.0f) { // pillarbox
                    sPosY = 0.0f;   // clamp y position
                    sHght = size.y; // reset height
                }
                if (sPosX < 0.0f) { // letterbox
                    sPosX = 0.0f;   // clamp x position
                    sWdth = size.x; // reset width
                }
                pos = ImVec2(sPosX, sPosY);
                size = ImVec2(sWdth, sHght);
            }
        } else { // in pixel perfect mode it's much easier
            const int factor = GetIntegerScaleFactor();
            float sPosX = floor(size.x / 2.0f - (interpreter->mCurDimensions.width * factor) / 2.0f);
            float sPosY = floor(size.y / 2.0f - (interpreter->mCurDimensions.height * factor) / 2.0f);
            pos = ImVec2(sPosX, sPosY);
            size = ImVec2(float(interpreter->mCurDimensions.width) * factor,
                          float(interpreter->mCurDimensions.height) * factor);
        }
    }
    uintptr_t fb = Ship::Context::GetRawInstance()->GetWindow()->GetGfxFrameBuffer();
    if (fb) {
        ImGui::SetCursorPos(pos);
        ImGui::Image(reinterpret_cast<ImTextureID>(fb), size);
    }

    ImGui::End();
}

void Fast3dGui::ApplyResolutionChanges() {
    ImVec2 size = ImGui::GetContentRegionAvail();

    const float aspectRatioX = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetFloat(
        CVAR_PREFIX_ADVANCED_RESOLUTION ".AspectRatioX", 16.0f);
    const float aspectRatioY = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetFloat(
        CVAR_PREFIX_ADVANCED_RESOLUTION ".AspectRatioY", 9.0f);
    const uint32_t verticalPixelCount = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
        CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalPixelCount", 480);
    const bool verticalResolutionToggle = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
        CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalResolutionToggle", 0);

    const bool aspectRatioIsEnabled = (aspectRatioX > 0.0f) && (aspectRatioY > 0.0f);

    constexpr uint32_t minResolutionWidth = 320;
    constexpr uint32_t minResolutionHeight = 240;
    constexpr uint32_t maxResolutionWidth = 8096;  // the renderer's actual limit is 16384
    constexpr uint32_t maxResolutionHeight = 4320; // on either axis. if you have the VRAM for it.
    uint32_t newWidth;
    uint32_t newHeight;
    const auto interpreter = mInterpreter.lock().get();
    interpreter->GetCurDimensions(&newWidth, &newHeight);

    if (verticalResolutionToggle) { // Use fixed vertical resolution
        if (aspectRatioIsEnabled) {
            newWidth = uint32_t(float(verticalPixelCount / aspectRatioY) * aspectRatioX);
        } else {
            newWidth = uint32_t(float(verticalPixelCount * size.x / size.y));
        }
        newHeight = verticalPixelCount;
    } else { // Use the window's resolution
        if (aspectRatioIsEnabled) {
            if (((float)interpreter->mGameWindowViewport.height / interpreter->mGameWindowViewport.width) <
                (aspectRatioY / aspectRatioX)) {
                // when pillarboxed
                newWidth = uint32_t(float(interpreter->mCurDimensions.height / aspectRatioY) * aspectRatioX);
            } else { // when letterboxed
                newHeight = uint32_t(float(interpreter->mCurDimensions.width / aspectRatioX) * aspectRatioY);
            }
        } // else, having both options turned off does nothing.
    }
    // clamp values to prevent renderer crash
    if (newWidth < minResolutionWidth) {
        newWidth = minResolutionWidth;
    }
    if (newHeight < minResolutionHeight) {
        newHeight = minResolutionHeight;
    }
    if (newWidth > maxResolutionWidth) {
        newWidth = maxResolutionWidth;
    }
    if (newHeight > maxResolutionHeight) {
        newHeight = maxResolutionHeight;
    }
    // apply new dimensions
    interpreter->mCurDimensions.width = newWidth;
    interpreter->mCurDimensions.height = newHeight;
    // centring the image is done in Fast3dGui::DrawGame().
}

int16_t Fast3dGui::GetIntegerScaleFactor() {
    const auto interpreter = mInterpreter.lock().get();
    if (!Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
            CVAR_PREFIX_ADVANCED_RESOLUTION ".IntegerScale.FitAutomatically", 0)) {
        int16_t factor = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
            CVAR_PREFIX_ADVANCED_RESOLUTION ".IntegerScale.Factor", 1);

        if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
                CVAR_PREFIX_ADVANCED_RESOLUTION ".IntegerScale.NeverExceedBounds", 1)) {
            if (((float)interpreter->mGameWindowViewport.height / interpreter->mGameWindowViewport.width) <
                ((float)interpreter->mCurDimensions.height / interpreter->mCurDimensions.width)) {
                if ((uint32_t)factor > interpreter->mGameWindowViewport.height / interpreter->mCurDimensions.height) {
                    factor = interpreter->mGameWindowViewport.height / interpreter->mCurDimensions.height;
                }
            } else {
                if ((uint32_t)factor > interpreter->mGameWindowViewport.width / interpreter->mCurDimensions.width) {
                    factor = interpreter->mGameWindowViewport.width / interpreter->mCurDimensions.width;
                }
            }
        }

        if (factor < 1) {
            factor = 1;
        }
        return factor;
    } else {
        int16_t factor = 1;

        if (((float)interpreter->mGameWindowViewport.height / interpreter->mGameWindowViewport.width) <
            ((float)interpreter->mCurDimensions.height / interpreter->mCurDimensions.width)) {
            factor = interpreter->mGameWindowViewport.height / interpreter->mCurDimensions.height;
        } else {
            factor = interpreter->mGameWindowViewport.width / interpreter->mCurDimensions.width;
        }

        factor += Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
            CVAR_PREFIX_ADVANCED_RESOLUTION ".IntegerScale.ExceedBoundsBy", 0);

        if (factor < 1) {
            factor = 1;
        }
        return factor;
    }
}

ImTextureID Fast3dGui::GetTextureById(int32_t id) {
    GfxRenderingAPI* api = mInterpreter.lock()->GetCurrentRenderingAPI();
    return api->GetTextureById(id);
}

bool Fast3dGui::HasTextureByName(const std::string& name) {
    return mGuiTextures.contains(name);
}

ImTextureID Fast3dGui::GetTextureByName(const std::string& name) {
    if (!HasTextureByName(name)) {
        return nullptr;
    }
    return GetTextureById(mGuiTextures[name].RendererTextureId);
}

ImVec2 Fast3dGui::GetTextureSize(const std::string& name) {
    if (!HasTextureByName(name)) {
        return ImVec2(0, 0);
    }
    return ImVec2(mGuiTextures[name].Width, mGuiTextures[name].Height);
}

void Fast3dGui::LoadTextureFromRawImage(const std::string& name, const std::string& path) {
    auto initData = std::make_shared<Ship::ResourceInitData>();
    initData->Format = RESOURCE_FORMAT_BINARY;
    initData->Type = static_cast<uint32_t>(RESOURCE_TYPE_GUI_TEXTURE);
    initData->ResourceVersion = 0;
    initData->Path = path;
    auto guiTexture = std::static_pointer_cast<Ship::GuiTexture>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(path, false, initData));

    LoadTextureFromResource(name, guiTexture);
}

void Fast3dGui::LoadTextureFromResource(const std::string& name, std::shared_ptr<Ship::GuiTexture> texture) {
    GfxRenderingAPI* api = mInterpreter.lock()->GetCurrentRenderingAPI();

    // TODO: Nothing ever unloads the texture from Fast3D here.
    texture->Metadata.RendererTextureId = api->NewTexture();
    api->SelectTexture(0, texture->Metadata.RendererTextureId);
    api->SetSamplerParameters(0, false, 0, 0);
    api->UploadTexture(texture->Data, texture->Metadata.Width, texture->Metadata.Height);

    mGuiTextures[name] = texture->Metadata;
}

void Fast3dGui::LoadGuiTexture(const std::string& name, const Fast::Texture& res, const std::string& palettePath,
                               const ImVec4& tint) {
    GfxRenderingAPI* api = mInterpreter.lock()->GetCurrentRenderingAPI();
    std::vector<uint8_t> texBuffer;
    texBuffer.reserve(res.Width * res.Height * 4);

    // For HD textures we need to load the buffer raw (similar to inside gfx_pp)
    if ((res.Flags & TEX_FLAG_LOAD_AS_RAW) != 0) {
        // Raw loading doesn't support TLUT textures
        if (res.Type == Fast::TextureType::Palette8bpp) {
            // TODO convert other image types
            SPDLOG_WARN("ImGui::ResourceLoad: Attempting to load unsupported image type");
            return;
        }

        texBuffer.assign(res.ImageData, res.ImageData + (res.Width * res.Height * 4));
    } else {
        switch (res.Type) {
            case Fast::TextureType::RGBA32bpp:
                texBuffer.assign(res.ImageData, res.ImageData + (res.Width * res.Height * 4));
                break;
            case Fast::TextureType::RGBA16bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i++) {
                    uint8_t b1 = res.ImageData[i * 2 + 0];
                    uint8_t b2 = res.ImageData[i * 2 + 1];
                    uint8_t r = (b1 >> 3) * 0xFF / 0x1F;
                    uint8_t g = (((b1 & 7) << 2) | (b2 >> 6)) * 0xFF / 0x1F;
                    uint8_t b = ((b2 >> 1) & 0x1F) * 0xFF / 0x1F;
                    uint8_t a = 0xFF * (b2 & 1);
                    texBuffer.push_back(r);
                    texBuffer.push_back(g);
                    texBuffer.push_back(b);
                    texBuffer.push_back(a);
                }
                break;
            }
            case Fast::TextureType::Palette4bpp: {
                if (palettePath.empty()) {
                    SPDLOG_WARN("ImGui::ResourceLoad: Palette4bpp requires a palette path for asset: {}", name);
                    return;
                }

                auto paletteRes = std::static_pointer_cast<Fast::Texture>(
                    Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(palettePath));

                if (!paletteRes || !paletteRes->ImageData) {
                    SPDLOG_WARN("ImGui::ResourceLoad: Failed to load palette asset from path: {}", palettePath);
                    return;
                }

                for (int32_t i = 0; i < res.Width * res.Height; i += 2) {
                    uint8_t b = res.ImageData[i / 2];

                    uint8_t idx1 = b >> 4;
                    uint8_t p1_b1 = paletteRes->ImageData[idx1 * 2 + 0];
                    uint8_t p1_b2 = paletteRes->ImageData[idx1 * 2 + 1];
                    uint8_t r1 = (p1_b1 >> 3) * 0xFF / 0x1F;
                    uint8_t g1 = (((p1_b1 & 7) << 2) | (p1_b2 >> 6)) * 0xFF / 0x1F;
                    uint8_t b1 = ((p1_b2 >> 1) & 0x1F) * 0xFF / 0x1F;
                    uint8_t a1 = 0xFF * (p1_b2 & 1);
                    texBuffer.push_back(r1);
                    texBuffer.push_back(g1);
                    texBuffer.push_back(b1);
                    texBuffer.push_back(a1);

                    uint8_t idx2 = b & 0xF;
                    uint8_t p2_b1 = paletteRes->ImageData[idx2 * 2 + 0];
                    uint8_t p2_b2 = paletteRes->ImageData[idx2 * 2 + 1];
                    uint8_t r2 = (p2_b1 >> 3) * 0xFF / 0x1F;
                    uint8_t g2 = (((p2_b1 & 7) << 2) | (p2_b2 >> 6)) * 0xFF / 0x1F;
                    uint8_t b2 = ((p2_b2 >> 1) & 0x1F) * 0xFF / 0x1F;
                    uint8_t a2 = 0xFF * (p2_b2 & 1);
                    texBuffer.push_back(r2);
                    texBuffer.push_back(g2);
                    texBuffer.push_back(b2);
                    texBuffer.push_back(a2);
                }
                break;
            }
            case Fast::TextureType::GrayscaleAlpha16bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i++) {
                    uint8_t color = res.ImageData[i * 2 + 0];
                    uint8_t alpha = res.ImageData[i * 2 + 1];
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(alpha);
                }
                break;
            }
            case Fast::TextureType::GrayscaleAlpha8bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i++) {
                    uint8_t ia = res.ImageData[i];
                    uint8_t color = ((ia >> 4) & 0xF) * 255 / 15;
                    uint8_t alpha = (ia & 0xF) * 255 / 15;
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(alpha);
                }
                break;
            }
            case Fast::TextureType::GrayscaleAlpha4bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i += 2) {
                    uint8_t b = res.ImageData[i / 2];

                    uint8_t ia4 = b >> 4;
                    uint8_t color = ((ia4 >> 1) & 0xF) * 255 / 0b111;
                    uint8_t alpha = (ia4 & 1) * 255;
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(alpha);

                    ia4 = b & 0xF;
                    color = ((ia4 >> 1) & 0xF) * 255 / 0b111;
                    alpha = (ia4 & 1) * 255;
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(alpha);
                }
                break;
            }
            case Fast::TextureType::Grayscale8bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i++) {
                    uint8_t ia = res.ImageData[i];
                    texBuffer.push_back(ia);
                    texBuffer.push_back(ia);
                    texBuffer.push_back(ia);
                    texBuffer.push_back(ia);
                }
                break;
            }
            case Fast::TextureType::Grayscale4bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i += 2) {
                    uint8_t b = res.ImageData[i / 2];

                    uint8_t ia4 = ((b >> 4) * 0xFF) / 0b1111;
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);

                    ia4 = ((b & 0xF) * 0xFF) / 0b1111;
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                }
                break;
            }
            default:
                // TODO convert other image types
                SPDLOG_WARN("ImGui::ResourceLoad: Attempting to load unsupported image type");
                return;
        }
    }

    for (size_t pixel = 0; pixel < texBuffer.size() / 4; pixel++) {
        texBuffer[pixel * 4 + 0] *= tint.x;
        texBuffer[pixel * 4 + 1] *= tint.y;
        texBuffer[pixel * 4 + 2] *= tint.z;
        texBuffer[pixel * 4 + 3] *= tint.w;
    }

    Ship::GuiTextureMetadata asset;
    asset.RendererTextureId = api->NewTexture();
    asset.Width = res.Width;
    asset.Height = res.Height;

    api->SelectTexture(0, asset.RendererTextureId);
    api->SetSamplerParameters(0, false, 0, 0);
    api->UploadTexture(texBuffer.data(), res.Width, res.Height);

    mGuiTextures[name] = asset;
}

void Fast3dGui::LoadGuiTexture(const std::string& name, const std::string& path, const std::string& palettePath,
                               const ImVec4& tint) {
    const auto res = static_cast<Fast::Texture*>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(path, true).get());

    LoadGuiTexture(name, *res, palettePath, tint);
}

void Fast3dGui::UnloadTexture(const std::string& name) {
    if (mGuiTextures.contains(name)) {
        Ship::GuiTextureMetadata tex = mGuiTextures[name];
        GfxRenderingAPI* api = mInterpreter.lock()->GetCurrentRenderingAPI();
        api->DeleteTexture(tex.RendererTextureId);
        mGuiTextures.erase(name);
    }
}

} // namespace Fast
