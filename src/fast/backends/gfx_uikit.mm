#include "fast/backends/gfx_uikit.h"

#if defined(__IOS__) && !defined(__VISIONOS__)

#import <UIKit/UIKit.h>
#import <CoreMotion/CoreMotion.h>
#include <objc/runtime.h>
#include <string>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <spdlog/spdlog.h>

namespace Fast {

namespace {
enum class Posture { Unknown, Portrait, InterfaceLandscapeRight, InterfaceLandscapeLeft };
enum class Stage { None, Geometry, Supported, Released, Failed };

constexpr double kGravityThreshold = 0.75;
constexpr Uint32 kOtherLandscapeHoldMs = 250;
constexpr Uint32 kStageTimeoutMs = 1000;

BOOL sLockWanted = YES;
bool sInstalled = false;
bool sLandscapeRequested = false;
CMMotionManager* sMotion = nil;
Posture sPosture = Posture::Unknown;
Uint32 sOtherLandscapeSince = 0;
std::string sOrientationsHint;

Stage sStage = Stage::None;
UIInterfaceOrientation sTarget = UIInterfaceOrientationUnknown;
Uint32 sTurnSince = 0;
Uint32 sStageSince = 0;
bool sStageRefused = false;
Posture sFailedPosture = Posture::Unknown;

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

UIInterfaceOrientation OrientationOf(Posture posture) {
    switch (posture) {
        case Posture::InterfaceLandscapeRight:
            return UIInterfaceOrientationLandscapeRight;
        case Posture::InterfaceLandscapeLeft:
            return UIInterfaceOrientationLandscapeLeft;
        default:
            return UIInterfaceOrientationUnknown;
    }
}

const char* StageName(Stage stage) {
    switch (stage) {
        case Stage::Geometry:
            return "geometry request, lock held";
        case Stage::Supported:
            return "supported orientations narrowed, lock held";
        case Stage::Released:
            return "geometry request, lock released";
        case Stage::Failed:
            return "failed";
        default:
            return "none";
    }
}

std::string Geometry(UIWindow* uiWindow) {
    UIWindowScene* scene = uiWindow.windowScene;
    const CGRect sceneBounds = scene.effectiveGeometry.coordinateSpace.bounds;
    const CGRect screen = uiWindow.screen.bounds;
    const CGRect frame = uiWindow.frame;
    UIView* root = uiWindow.rootViewController.view;
    const CGRect view = root.bounds;
    const CGAffineTransform t = root.transform;
    return fmt::format("scene {}x{}, screen {}x{}, window {}x{} at {},{}, view {}x{}, "
                       "transform [{:.2f} {:.2f} {:.2f} {:.2f}]",
                       (int)sceneBounds.size.width, (int)sceneBounds.size.height, (int)screen.size.width,
                       (int)screen.size.height, (int)frame.size.width, (int)frame.size.height, (int)frame.origin.x,
                       (int)frame.origin.y, (int)view.size.width, (int)view.size.height, t.a, t.b, t.c, t.d);
}

bool EngageIsSafe(UIInterfaceOrientation orientation) {
    const long device = (long)UIDevice.currentDevice.orientation;
    const bool deviceLandscape = device == (long)UIInterfaceOrientationLandscapeLeft ||
                                 device == (long)UIInterfaceOrientationLandscapeRight;
    return !deviceLandscape || device == (long)orientation;
}

void SetLock(UIWindow* uiWindow, BOOL wanted, const char* why) {
    if (wanted == sLockWanted) {
        return;
    }
    sLockWanted = wanted;
    [uiWindow.rootViewController setNeedsUpdateOfPrefersInterfaceOrientationLocked];
    SPDLOG_INFO("Orientation lock {} ({}): interface orientation {}, device orientation {}, posture {}",
                wanted ? "requested" : "released", why,
                (long)uiWindow.windowScene.effectiveGeometry.interfaceOrientation,
                (long)UIDevice.currentDevice.orientation, (int)sPosture);
}

void RequestGeometry(UIWindowScene* scene, UIInterfaceOrientationMask mask) {
    UIWindowSceneGeometryPreferencesIOS* preferences =
        [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
    [scene requestGeometryUpdateWithPreferences:preferences
                                   errorHandler:^(NSError* error) {
                                       sStageRefused = true;
                                       SPDLOG_WARN("Geometry request for mask {:#x} refused: {}", (unsigned long)mask,
                                                   error.localizedDescription.UTF8String);
                                   }];
}

void RestoreOrientationsHint() {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, sOrientationsHint.c_str());
}

void EnterStage(UIWindow* uiWindow, Stage stage) {
    UIWindowScene* scene = uiWindow.windowScene;
    sStage = stage;
    sStageSince = SDL_GetTicks();
    sStageRefused = false;
    SPDLOG_INFO("Turn to interface orientation {}: {}", (long)sTarget, StageName(stage));
    switch (stage) {
        case Stage::Geometry:
            RequestGeometry(scene, (UIInterfaceOrientationMask)(1 << sTarget));
            break;
        case Stage::Supported:
            SDL_SetHint(SDL_HINT_ORIENTATIONS,
                        sTarget == UIInterfaceOrientationLandscapeLeft ? "LandscapeLeft" : "LandscapeRight");
            [uiWindow.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
            break;
        case Stage::Released:
            RestoreOrientationsHint();
            SetLock(uiWindow, NO, "turn");
            RequestGeometry(scene, (UIInterfaceOrientationMask)(1 << sTarget));
            break;
        case Stage::Failed:
            RestoreOrientationsHint();
            sFailedPosture = sPosture;
            SPDLOG_WARN("Turn to interface orientation {} failed after {} ms; no new turn until the posture changes",
                        (long)sTarget, SDL_GetTicks() - sTurnSince);
            sStage = Stage::None;
            break;
        default:
            break;
    }
}

void AdvanceTurn(UIWindow* uiWindow, UIInterfaceOrientation orientation) {
    const Uint32 now = SDL_GetTicks();
    if (orientation == sTarget) {
        RestoreOrientationsHint();
        SPDLOG_INFO("Turn to interface orientation {} done after {} ms via {}: device orientation {}, orientation lock "
                    "{}, {}",
                    (long)sTarget, now - sTurnSince, StageName(sStage), (long)UIDevice.currentDevice.orientation,
                    uiWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked ? "held" : "not held",
                    Geometry(uiWindow));
        sStage = Stage::None;
        return;
    }
    if (!sStageRefused && now - sStageSince < kStageTimeoutMs) {
        return;
    }
    switch (sStage) {
        case Stage::Geometry:
            EnterStage(uiWindow, Stage::Supported);
            break;
        case Stage::Supported:
            EnterStage(uiWindow, Stage::Released);
            break;
        case Stage::Released:
            EnterStage(uiWindow, Stage::Failed);
            break;
        default:
            break;
    }
}
} // namespace

void UIKitRequestOrientationLock(SDL_Window* window) {
    UIViewController* controller = WindowOf(window).rootViewController;
    if (controller == nil) {
        SPDLOG_WARN("No UIKit view controller; the orientation lock is not requested");
        return;
    }
    if (@available(iOS 26.0, *)) {
        const char* hint = SDL_GetHint(SDL_HINT_ORIENTATIONS);
        sOrientationsHint = hint != nullptr ? hint : "LandscapeLeft LandscapeRight";
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
            sFailedPosture = Posture::Unknown;
            SPDLOG_INFO("Posture {} (gravity {:.2f},{:.2f}): device orientation {}, interface orientation {}, "
                        "orientation lock {}, turn stage {}, {}",
                        (int)posture, g.x, g.y, (long)UIDevice.currentDevice.orientation, (long)orientation,
                        scene.effectiveGeometry.isInterfaceOrientationLocked ? "held" : "not held", StageName(sStage),
                        Geometry(uiWindow));
        }

        const bool landscape = UIInterfaceOrientationIsLandscape(orientation);
        if (!landscape && !sLandscapeRequested) {
            sLandscapeRequested = true;
            SPDLOG_INFO("Scene in interface orientation {}: landscape geometry requested", (long)orientation);
            RequestGeometry(scene, UIInterfaceOrientationMaskLandscape);
        } else if (landscape) {
            sLandscapeRequested = false;
        }

        if (sStage != Stage::None) {
            AdvanceTurn(uiWindow, orientation);
        }

        const UIInterfaceOrientation wanted = OrientationOf(sPosture);
        const bool otherLandscape = landscape && wanted != UIInterfaceOrientationUnknown && wanted != orientation;
        const Uint32 now = SDL_GetTicks();
        if (!otherLandscape) {
            sOtherLandscapeSince = 0;
        } else if (sOtherLandscapeSince == 0) {
            sOtherLandscapeSince = now;
        }
        if (sStage == Stage::None && otherLandscape && sPosture != sFailedPosture &&
            now - sOtherLandscapeSince >= kOtherLandscapeHoldMs) {
            sTarget = wanted;
            sTurnSince = now;
            EnterStage(uiWindow, Stage::Geometry);
        }

        if (!sLockWanted && sStage == Stage::None && EngageIsSafe(orientation)) {
            SetLock(uiWindow, YES, "scene and device agree");
        }
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
