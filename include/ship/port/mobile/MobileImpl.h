#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

union SDL_Event;

namespace Ship {

class Mobile {
  public:
    static void SyncTextInput();
    static void DrawScreenKeyboard();
    static bool HandleScreenKeyboardEvent(const SDL_Event* event);
};
}; // namespace Ship
