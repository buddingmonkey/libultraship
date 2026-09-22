#include "fast/backends/gfx_uikit.h"

#if defined(__IOS__) && !defined(__VISIONOS__)

#import <UIKit/UIKit.h>
#import <CoreMotion/CoreMotion.h>
#include <objc/runtime.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <spdlog/spdlog.h>

namespace Fast {

namespace {
enum class Posture { Unknown, Portrait, InterfaceLandscapeRight, InterfaceLandscapeLeft };

constexpr double kGravityThreshold = 0.75;
constexpr Uint32 kOtherLandscapeHoldMs = 250;

BOOL sLockWanted = YES;
bool sInstalled = false;
bool sLandscapeRequested = false;
CMMotionManager* sMotion = nil;
Posture sPosture = Posture::Unknown;
Uint32 sOtherLandscapeSince = 0;

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

Posture ReadPosture(CMAcceleration g) {
    if (g.x < -kGravityThreshold) {
        return Posture::InterfaceLandscapeRight;
    }
    if (g.x > kGravityThreshold) {
        return Posture::InterfaceLandscapeLeft;
    }
    if (g.y < -kGravityThreshold || g.y > kGravityThreshold) {
        return Posture::Portrait;
    }
    return Posture::Unknown;
}

bool Matches(Posture posture, UIInterfaceOrientation orientation) {
    return (posture == Posture::InterfaceLandscapeRight && orientation == UIInterfaceOrientationLandscapeRight) ||
           (posture == Posture::InterfaceLandscapeLeft && orientation == UIInterfaceOrientationLandscapeLeft);
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
        [UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
        sMotion = [[CMMotionManager alloc] init];
        if (sMotion.accelerometerAvailable) {
            sMotion.accelerometerUpdateInterval = 0.05;
            [sMotion startAccelerometerUpdates];
        } else {
            SPDLOG_WARN("No accelerometer; the orientation lock stays held");
        }
        sInstalled = true;
    }
}

void UIKitUpdateOrientationLock(SDL_Window* window) {
    if (!sInstalled || sMotion.accelerometerData == nil) {
        return;
    }
    if (@available(iOS 26.0, *)) {
        UIWindow* uiWindow = WindowOf(window);
        UIWindowScene* scene = uiWindow.windowScene;
        if (scene == nil) {
            return;
        }
        const CMAcceleration g = sMotion.accelerometerData.acceleration;
        const UIInterfaceOrientation orientation = scene.effectiveGeometry.interfaceOrientation;
        const Posture posture = ReadPosture(g);

        if (posture != sPosture && posture != Posture::Unknown) {
            sPosture = posture;
            const CGRect bounds = uiWindow.bounds;
            SPDLOG_INFO("Posture {} (gravity {:.2f},{:.2f}): device orientation {}, interface orientation {}, "
                        "window {}x{}, orientation lock {}",
                        (int)posture, g.x, g.y, (long)UIDevice.currentDevice.orientation, (long)orientation,
                        (int)bounds.size.width, (int)bounds.size.height,
                        scene.effectiveGeometry.isInterfaceOrientationLocked ? "held" : "not held");
        }

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

        const bool otherLandscape = landscape && (sPosture == Posture::InterfaceLandscapeRight ||
                                                  sPosture == Posture::InterfaceLandscapeLeft) &&
                                    !Matches(sPosture, orientation);
        const Uint32 now = SDL_GetTicks();
        if (!otherLandscape) {
            sOtherLandscapeSince = 0;
        } else if (sOtherLandscapeSince == 0) {
            sOtherLandscapeSince = now;
        }
        const BOOL wanted = !(otherLandscape && now - sOtherLandscapeSince >= kOtherLandscapeHoldMs);
        if (wanted == sLockWanted) {
            return;
        }
        sLockWanted = wanted;
        [uiWindow.rootViewController setNeedsUpdateOfPrefersInterfaceOrientationLocked];
        SPDLOG_INFO("Orientation lock {}: interface orientation {}, posture {}", wanted ? "requested" : "released",
                    (long)orientation, (int)sPosture);
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
