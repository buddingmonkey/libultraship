#pragma once

#if defined(__IOS__) && !defined(__VISIONOS__)

struct SDL_Window;

namespace Fast {
void UIKitRequestOrientationLock(SDL_Window* window);
void UIKitUpdateOrientationLock(SDL_Window* window);
void UIKitLogOrientation(SDL_Window* window, int width, int height);
} // namespace Fast

#endif
