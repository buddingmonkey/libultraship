#include "fast/backends/gfx_uikit.h"

#if defined(__IOS__) && !defined(__VISIONOS__)

#import <UIKit/UIKit.h>
#include <objc/runtime.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <spdlog/spdlog.h>

namespace Fast {

namespace {
BOOL PrefersInterfaceOrientationLocked(id, SEL) {
    return YES;
}

UIWindow* WindowOf(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT) {
        return nil;
    }
    return info.info.uikit.window;
}
} // namespace

void UIKitRequestOrientationLock(SDL_Window* window) {
    UIViewController* controller = WindowOf(window).rootViewController;
    if (controller == nil) {
        SPDLOG_WARN("No UIKit view controller; the orientation lock is not requested");
        return;
    }
    if (@available(iOS 26.0, *)) {
        class_addMethod([controller class], @selector(prefersInterfaceOrientationLocked),
                        (IMP)PrefersInterfaceOrientationLocked, "B@:");
        [controller setNeedsUpdateOfPrefersInterfaceOrientationLocked];
    }
}

void UIKitLogOrientation(SDL_Window* window, int width, int height) {
    UIWindowScene* scene = WindowOf(window).windowScene;
    if (scene == nil) {
        return;
    }
    UIWindowSceneGeometry* geometry = scene.effectiveGeometry;
    const char* lock = "not available";
    if (@available(iOS 26.0, *)) {
        lock = geometry.isInterfaceOrientationLocked ? "held" : "not held";
    }
    SPDLOG_INFO("Window {}x{}: interface orientation {}, orientation lock {}", width, height,
                (long)geometry.interfaceOrientation, lock);
}

} // namespace Fast

#endif
