#include "fast/backends/gfx_uikit.h"

#if defined(__IOS__) && !defined(__VISIONOS__)

#import <UIKit/UIKit.h>
#include <objc/runtime.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <spdlog/spdlog.h>

namespace Fast {

namespace {
BOOL sLockWanted = NO;
bool sInstalled = false;
bool sLandscapeRequested = false;

BOOL PrefersInterfaceOrientationLocked(id, SEL) {
    return sLockWanted;
}

UIWindow* WindowOf(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT) {
        return nil;
    }
    return info.info.uikit.window;
}

bool DeviceFacesOtherLandscape(UIDeviceOrientation device, UIInterfaceOrientation scene) {
    return (device == UIDeviceOrientationLandscapeLeft && scene != UIInterfaceOrientationLandscapeRight) ||
           (device == UIDeviceOrientationLandscapeRight && scene != UIInterfaceOrientationLandscapeLeft);
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
        [UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
        sInstalled = true;
        UIKitUpdateOrientationLock(window);
    }
}

void UIKitUpdateOrientationLock(SDL_Window* window) {
    if (!sInstalled) {
        return;
    }
    if (@available(iOS 26.0, *)) {
        UIWindow* uiWindow = WindowOf(window);
        UIWindowScene* scene = uiWindow.windowScene;
        if (scene == nil) {
            return;
        }
        const UIInterfaceOrientation orientation = scene.effectiveGeometry.interfaceOrientation;
        const UIDeviceOrientation device = UIDevice.currentDevice.orientation;
        const bool landscape = UIInterfaceOrientationIsLandscape(orientation);

        if (!landscape && !sLandscapeRequested) {
            sLandscapeRequested = true;
            UIWindowSceneGeometryPreferencesIOS* preferences = [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
            [scene requestGeometryUpdateWithPreferences:preferences
                                           errorHandler:^(NSError* error) {
                                               SPDLOG_WARN("Landscape geometry request refused: {}",
                                                           error.localizedDescription.UTF8String);
                                           }];
        } else if (landscape) {
            sLandscapeRequested = false;
        }

        const BOOL wanted = landscape && !DeviceFacesOtherLandscape(device, orientation);
        if (wanted == sLockWanted) {
            return;
        }
        sLockWanted = wanted;
        [uiWindow.rootViewController setNeedsUpdateOfPrefersInterfaceOrientationLocked];
        SPDLOG_INFO("Orientation lock {}: interface orientation {}, device orientation {}",
                    wanted ? "requested" : "released", (long)orientation, (long)device);
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
