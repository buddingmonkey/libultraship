#if defined(__ANDROID__) || defined(__IOS__)
#include "ship/port/mobile/MobileImpl.h"
#include <SDL2/SDL.h>

#ifdef __ANDROID__
#include <jni.h>
#endif

#include <imgui_internal.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <vector>

#ifdef ENABLE_OPENXR
#include "fast/backends/gfx_xr_view.h"
#endif

namespace {

bool sSystemKeyboardShown = false;

enum class SystemKeyboard { Unknown, Works, Missing };
SystemKeyboard sSystemKeyboard = SystemKeyboard::Unknown;
Uint32 sSystemKeyboardAskedAt = 0;
constexpr Uint32 kSystemKeyboardWaitMs = 3000;
constexpr int kResultUnchangedShown = 0;
constexpr int kResultShown = 2;

bool IsHeadset() {
#ifdef ENABLE_OPENXR
    return Fast::IsXrPresenting();
#else
    return false;
#endif
}

bool UseScreenKeyboard() {
    return IsHeadset() && sSystemKeyboard == SystemKeyboard::Missing;
}

#ifdef __ANDROID__
bool CallActivity(const char* name, const char* signature, int* result) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) {
        return false;
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(cls, name, signature);
    bool called = false;
    if (method == nullptr) {
        env->ExceptionClear();
    } else if (result != nullptr) {
        *result = env->CallIntMethod(activity, method);
        called = true;
    } else {
        env->CallVoidMethod(activity, method);
        called = true;
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return called;
}
#endif

bool ProbeSystemKeyboard() {
#ifdef __ANDROID__
    return CallActivity("probeSoftKeyboard", "()V", nullptr);
#else
    return false;
#endif
}

int SystemKeyboardResult() {
    int result = -1;
#ifdef __ANDROID__
    CallActivity("softKeyboardResult", "()I", &result);
#endif
    return result;
}

enum class KeyKind { Character, Shift, Backspace, Space, Done };

struct ScreenKey {
    ImVec2 min;
    ImVec2 max;
    KeyKind kind;
    char character;
};

std::vector<ScreenKey> sKeys;
ImVec2 sPanelMin;
ImVec2 sPanelMax;
bool sPanelVisible = false;
bool sShift = false;
bool sPressConsumed = false;
int sPressedKey = -1;

void LayoutScreenKeyboard(const ImVec2& origin, const ImVec2& size) {
    static const char* const kRows[] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm" };
    const float panelWidth = std::min(size.x * 0.9f, size.y * 1.18f);
    const float pad = panelWidth * 0.012f;
    const float key = (panelWidth - pad * 11.0f) / 10.0f;
    const float panelHeight = key * 5.0f + pad * 6.0f;
    sPanelMin = ImVec2(origin.x + (size.x - panelWidth) * 0.5f, origin.y + size.y - panelHeight - pad * 2.0f);
    sPanelMax = ImVec2(sPanelMin.x + panelWidth, sPanelMin.y + panelHeight);

    sKeys.clear();
    auto add = [&](float x, float y, float width, KeyKind kind, char character) {
        sKeys.push_back({ ImVec2(x, y), ImVec2(x + width, y + key), kind, character });
    };
    for (int row = 0; row < 4; row++) {
        const float y = sPanelMin.y + pad + (key + pad) * row;
        const int count = (int)strlen(kRows[row]);
        float x = sPanelMin.x + pad + (10 - count) * (key + pad) * 0.5f;
        if (row == 3) {
            const float wide = key * 1.5f;
            add(sPanelMin.x + pad, y, wide, KeyKind::Shift, 0);
            add(sPanelMax.x - pad - wide, y, wide, KeyKind::Backspace, 0);
        }
        for (int i = 0; i < count; i++) {
            add(x, y, key, KeyKind::Character, kRows[row][i]);
            x += key + pad;
        }
    }
    const float y = sPanelMin.y + pad + (key + pad) * 4;
    const float space = key * 6.0f + pad * 5.0f;
    const float done = key * 2.0f + pad;
    const float x = sPanelMin.x + (panelWidth - space - done - pad) * 0.5f;
    add(x, y, space, KeyKind::Space, ' ');
    add(x + space + pad, y, done, KeyKind::Done, 0);
}

int KeyAt(float x, float y) {
    for (size_t i = 0; i < sKeys.size(); i++) {
        if (x >= sKeys[i].min.x && x < sKeys[i].max.x && y >= sKeys[i].min.y && y < sKeys[i].max.y) {
            return (int)i;
        }
    }
    return -1;
}

void PressKey(const ScreenKey& key) {
    ImGuiIO& io = ImGui::GetIO();
    switch (key.kind) {
        case KeyKind::Character:
            io.AddInputCharacter((unsigned int)(sShift ? std::toupper((unsigned char)key.character) : key.character));
            sShift = false;
            break;
        case KeyKind::Space:
            io.AddInputCharacter(' ');
            break;
        case KeyKind::Shift:
            sShift = !sShift;
            break;
        case KeyKind::Backspace:
            io.AddKeyEvent(ImGuiKey_Backspace, true);
            io.AddKeyEvent(ImGuiKey_Backspace, false);
            break;
        case KeyKind::Done:
            io.AddKeyEvent(ImGuiKey_Enter, true);
            io.AddKeyEvent(ImGuiKey_Enter, false);
            break;
    }
}

const char* KeyLabel(const ScreenKey& key, char* buffer) {
    switch (key.kind) {
        case KeyKind::Shift:
            return "Shift";
        case KeyKind::Backspace:
            return "Del";
        case KeyKind::Space:
            return "Space";
        case KeyKind::Done:
            return "Done";
        default:
            buffer[0] = sShift ? (char)std::toupper((unsigned char)key.character) : key.character;
            buffer[1] = '\0';
            return buffer;
    }
}

} // namespace

void Ship::Mobile::SyncTextInput() {
    const bool wantsTextInput = ImGui::GetIO().WantTextInput;
    if (UseScreenKeyboard()) {
        if (sSystemKeyboardShown) {
            sSystemKeyboardShown = false;
            SDL_StopTextInput();
        }
        return;
    }

    if (wantsTextInput) {
        if (!sSystemKeyboardShown) {
            if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetActiveID())) {
                state->ClearText();
            }
            sSystemKeyboardShown = true;
            SDL_StartTextInput();
            if (IsHeadset() && sSystemKeyboard == SystemKeyboard::Unknown) {
                if (ProbeSystemKeyboard()) {
                    sSystemKeyboardAskedAt = SDL_GetTicks();
                } else {
                    sSystemKeyboard = SystemKeyboard::Missing;
                    sSystemKeyboardShown = false;
                    SDL_StopTextInput();
                }
            }
        } else if (sSystemKeyboardAskedAt != 0) {
            const int result = SystemKeyboardResult();
            const Uint32 waited = SDL_GetTicks() - sSystemKeyboardAskedAt;
            if (result == kResultShown || result == kResultUnchangedShown) {
                sSystemKeyboardAskedAt = 0;
                sSystemKeyboard = SystemKeyboard::Works;
                SPDLOG_INFO("The system keyboard is shown ({} ms)", waited);
            } else if (result >= 0 || waited > kSystemKeyboardWaitMs) {
                sSystemKeyboardAskedAt = 0;
                sSystemKeyboard = SystemKeyboard::Missing;
                sSystemKeyboardShown = false;
                SDL_StopTextInput();
                SPDLOG_INFO("No system keyboard (result {}, {} ms); drawing the menu keyboard", result, waited);
            }
        } else if (!SDL_IsTextInputActive()) {
            sSystemKeyboardShown = false;
            ImGui::ClearActiveID();
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // The system can hide the keyboard (Back) without a text input stop; a tap on the field shows it again.
            SDL_StartTextInput();
        }
    } else if (sSystemKeyboardShown) {
        sSystemKeyboardShown = false;
        sSystemKeyboardAskedAt = 0;
        SDL_StopTextInput();
    }
}

void Ship::Mobile::DrawScreenKeyboard() {
    sPanelVisible = UseScreenKeyboard() && ImGui::GetIO().WantTextInput;
    if (!sPanelVisible) {
        sShift = false;
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    LayoutScreenKeyboard(viewport->Pos, viewport->Size);

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float rounding = (sKeys[0].max.y - sKeys[0].min.y) * 0.15f;
    const float fontSize = (sKeys[0].max.y - sKeys[0].min.y) * 0.45f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const int hovered = KeyAt(mouse.x, mouse.y);

    draw->AddRectFilled(sPanelMin, sPanelMax, IM_COL32(20, 22, 30, 240), rounding);
    for (size_t i = 0; i < sKeys.size(); i++) {
        const ScreenKey& key = sKeys[i];
        ImU32 color = IM_COL32(70, 74, 90, 255);
        if ((int)i == sPressedKey) {
            color = IM_COL32(40, 90, 200, 255);
        } else if ((int)i == hovered) {
            color = IM_COL32(100, 106, 128, 255);
        } else if (key.kind == KeyKind::Shift && sShift) {
            color = IM_COL32(40, 90, 200, 255);
        }
        draw->AddRectFilled(key.min, key.max, color, rounding);

        char buffer[2];
        const char* label = KeyLabel(key, buffer);
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
        const ImVec2 textPos((key.min.x + key.max.x - textSize.x) * 0.5f, (key.min.y + key.max.y - textSize.y) * 0.5f);
        draw->AddText(font, fontSize, textPos, IM_COL32(235, 235, 240, 255), label);
    }
}

bool Ship::Mobile::HandleScreenKeyboardEvent(const SDL_Event* event) {
    if (event->type == SDL_MOUSEBUTTONUP && sPressConsumed) {
        sPressConsumed = false;
        sPressedKey = -1;
        return true;
    }
    if (!sPanelVisible || event->type != SDL_MOUSEBUTTONDOWN) {
        return false;
    }
    const float x = (float)event->button.x;
    const float y = (float)event->button.y;
    if (x < sPanelMin.x || x >= sPanelMax.x || y < sPanelMin.y || y >= sPanelMax.y) {
        return false;
    }
    sPressConsumed = true;
    sPressedKey = KeyAt(x, y);
    if (sPressedKey >= 0) {
        PressKey(sKeys[sPressedKey]);
    }
    return true;
}
#endif
