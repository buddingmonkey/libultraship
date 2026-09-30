#include "fast/backends/gfx_uikit.h"

#if defined(__IOS__) && !defined(__VISIONOS__)

#import <UIKit/UIKit.h>
#include <objc/runtime.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <spdlog/spdlog.h>

#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
#define LUS_UIKIT_ORIENTATION_LOCK 1
#endif

@interface SDLUIKitSceneDelegate : NSObject
@end

namespace Fast {
namespace {
constexpr CGFloat kWidePhoneDisplayPt = 600.0;
constexpr NSUInteger kFreeMask = UIInterfaceOrientationMaskAll;

CGSize SceneSizeOf(UIWindowScene* scene) {
    if (scene == nil) {
        return CGSizeZero;
    }
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0, *)) {
        return scene.effectiveGeometry.coordinateSpace.bounds.size;
    }
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return scene.coordinateSpace.bounds.size;
#pragma clang diagnostic pop
}

bool WidePhoneDisplay(CGSize sceneSize) {
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone &&
           MIN(sceneSize.width, sceneSize.height) >= kWidePhoneDisplayPt;
}

NSUInteger MaskForScene(UIWindowScene* scene, const char* who) {
    static NSUInteger sLastMask = 0;
    const CGSize size = SceneSizeOf(scene);
    const NSUInteger mask = WidePhoneDisplay(size) ? kFreeMask : UIInterfaceOrientationMaskLandscape;
    if (mask != sLastMask) {
        sLastMask = mask;
        SPDLOG_INFO("Supported orientations mask {:#x} from the {} for a {}x{} pt scene", mask, who, (int)size.width,
                    (int)size.height);
    }
    return mask;
}
} // namespace
} // namespace Fast

@implementation SDLUIKitSceneDelegate (LusOrientations)
- (UIInterfaceOrientationMask)supportedInterfaceOrientationsForWindowScene:(UIWindowScene*)windowScene {
    return Fast::MaskForScene(windowScene, "scene delegate");
}
@end

namespace Fast {

namespace {
constexpr int64_t kRelockDelayNs = 1000 * NSEC_PER_MSEC;
constexpr const char* kFreeOrientationsHint = "Portrait PortraitUpsideDown LandscapeLeft LandscapeRight";

BOOL sLockWanted = NO;
BOOL sFree = NO;
bool sModeApplied = false;
UIWindow* sWindow = nil;
IMP sOriginalTransition = nullptr;
IMP sOriginalSupported = nullptr;
IMP sOriginalGeometryUpdate = nullptr;
uint64_t sTurn = 0;
long sLastLandscape = (long)UIInterfaceOrientationLandscapeRight;

BOOL PrefersInterfaceOrientationLocked(id, SEL) {
    return sLockWanted;
}

NSUInteger SupportedOrientationsHook(id self, SEL cmd) {
    // SDL drops PortraitUpsideDown on the phone idiom; the wide inner display of a foldable is held both ways.
    if (sFree) {
        return kFreeMask;
    }
    return ((NSUInteger(*)(id, SEL))sOriginalSupported)(self, cmd);
}

UIWindow* WindowOf(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT) {
        return nil;
    }
    return info.info.uikit.window;
}

long DeviceOrientation() {
    return (long)UIDevice.currentDevice.orientation;
}

long InterfaceOrientation() {
    return (long)sWindow.windowScene.effectiveGeometry.interfaceOrientation;
}

bool IsLandscape(long orientation) {
    return orientation == (long)UIInterfaceOrientationLandscapeLeft ||
           orientation == (long)UIInterfaceOrientationLandscapeRight;
}

CGSize SceneSize() {
    return sWindow.windowScene != nil ? SceneSizeOf(sWindow.windowScene) : sWindow.bounds.size;
}

CGSize ScreenSize() {
    UIScreen* screen = sWindow.windowScene.screen;
    return screen != nil ? screen.bounds.size : CGSizeZero;
}

void LogMasks(const char* why) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const NSUInteger app = [UIApplication.sharedApplication supportedInterfaceOrientationsForWindow:sWindow];
#pragma clang diagnostic pop
    const NSUInteger controller = [sWindow.rootViewController supportedInterfaceOrientations];
    SPDLOG_INFO("Orientation masks ({}): application {:#x}, view controller {:#x}, hint {}", why, app, controller,
                SDL_GetHint(SDL_HINT_ORIENTATIONS) != nullptr ? SDL_GetHint(SDL_HINT_ORIENTATIONS) : "none");
}

void LogScene(const char* why, CGSize sceneSize) {
    UIWindowScene* scene = sWindow.windowScene;
    const CGSize screen = ScreenSize();
    const CGSize native = scene.screen != nil ? scene.screen.nativeBounds.size : CGSizeZero;
    UITraitCollection* traits = scene.traitCollection;
    SPDLOG_INFO("Scene ({}): {}x{} pt, screen {}x{} pt, native {}x{} px, size classes h{} v{}, idiom {}, interface "
                "orientation {}, device orientation {}, lock {}, portrait {}",
                why, (int)sceneSize.width, (int)sceneSize.height, (int)screen.width, (int)screen.height,
                (int)native.width, (int)native.height, (long)traits.horizontalSizeClass,
                (long)traits.verticalSizeClass, (long)UIDevice.currentDevice.userInterfaceIdiom,
                InterfaceOrientation(), DeviceOrientation(), sLockWanted ? "wanted" : "released",
                sFree ? "allowed" : "blocked");
}

void SetLock(BOOL wanted, const char* why) {
#ifdef LUS_UIKIT_ORIENTATION_LOCK
    if (@available(iOS 26.0, *)) {
        sLockWanted = wanted;
        [sWindow.rootViewController setNeedsUpdateOfPrefersInterfaceOrientationLocked];
        SPDLOG_INFO("Orientation lock {} ({}): interface orientation {}, device orientation {}",
                    wanted ? "requested" : "released", why, InterfaceOrientation(), DeviceOrientation());
    }
#endif
}

void SupportHint(const char* hint, const char* why) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, hint);
    [sWindow.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
    SPDLOG_INFO("Supported orientations {} ({})", hint, why);
}

void SupportOnly(long orientation, const char* why) {
    if (IsLandscape(orientation)) {
        sLastLandscape = orientation;
    }
    SupportHint(orientation == (long)UIInterfaceOrientationLandscapeLeft ? "LandscapeLeft" : "LandscapeRight", why);
}

void RelockAfterTurn() {
    const uint64_t turn = sTurn;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRelockDelayNs), dispatch_get_main_queue(), ^{
        if (turn == sTurn && !sFree) {
            SetLock(YES, "turn complete");
        }
    });
}

void ApplyDisplayMode(CGSize sceneSize, const char* why, bool atLaunch) {
    const BOOL free = WidePhoneDisplay(sceneSize) ? YES : NO;
    LogScene(why, sceneSize);
    if (sModeApplied && free == sFree) {
        return;
    }
    sModeApplied = true;
    sFree = free;
    sTurn++;
    const long interface = InterfaceOrientation();
    if (free) {
        SetLock(NO, why);
        SupportHint(kFreeOrientationsHint, why);
        LogMasks(why);
        if (@available(iOS 16.0, *)) {
            UIWindowSceneGeometryPreferencesIOS* preferences =
                [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:kFreeMask];
            [sWindow.windowScene requestGeometryUpdateWithPreferences:preferences
                                                         errorHandler:^(NSError* error) {
                                                             SPDLOG_WARN("Geometry request refused: {}",
                                                                         error.localizedDescription.UTF8String);
                                                         }];
        }
        return;
    }
    SupportOnly(IsLandscape(interface) ? interface : sLastLandscape, why);
    LogMasks(why);
    if (atLaunch || IsLandscape(interface)) {
        SetLock(YES, why);
    } else {
        RelockAfterTurn();
    }
}

void TransitionHook(id self, SEL cmd, CGSize size, id<UIViewControllerTransitionCoordinator> coordinator) {
    SPDLOG_INFO("Rotation transition to {}x{}, animated {}, {:.2f} s: interface orientation {}, device orientation {}",
                (int)size.width, (int)size.height, coordinator.animated ? "yes" : "no", coordinator.transitionDuration,
                InterfaceOrientation(), DeviceOrientation());
    ((void (*)(id, SEL, CGSize, id))sOriginalTransition)(self, cmd, size, coordinator);
    dispatch_async(dispatch_get_main_queue(), ^{
        ApplyDisplayMode(size, "transition", false);
    });
}

#ifdef LUS_UIKIT_ORIENTATION_LOCK
void GeometryUpdateHook(id self, SEL cmd, UIWindowScene* scene, UIWindowSceneGeometry* previous) {
    if (sOriginalGeometryUpdate != nullptr) {
        ((void (*)(id, SEL, UIWindowScene*, UIWindowSceneGeometry*))sOriginalGeometryUpdate)(self, cmd, scene,
                                                                                            previous);
    }
    if (@available(iOS 26.0, *)) {
        const CGSize was = previous.coordinateSpace.bounds.size;
        SPDLOG_INFO("Scene geometry update: was {}x{} pt, interface orientation {}", (int)was.width, (int)was.height,
                    (long)previous.interfaceOrientation);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        ApplyDisplayMode(SceneSize(), "geometry update", false);
    });
}
#endif

void OnDeviceOrientation() {
    const long device = DeviceOrientation();
    if (sFree) {
        if (IsLandscape(device)) {
            sLastLandscape = device;
        }
        SPDLOG_INFO("Device orientation {} with portrait allowed: interface orientation {}", device,
                    InterfaceOrientation());
        return;
    }
    if (!IsLandscape(device)) {
        if (!sLockWanted) {
            sTurn++;
            SetLock(YES, "device left landscape");
        } else {
            SPDLOG_INFO("Device orientation {} while locked: interface orientation {} stays", device,
                        InterfaceOrientation());
        }
        return;
    }
    sTurn++;
    SetLock(NO, "device in landscape");
    SupportOnly(device, "device in landscape");
    RelockAfterTurn();
}

void LogDelegates() {
    id appDelegate = UIApplication.sharedApplication.delegate;
    id sceneDelegate = sWindow.windowScene.delegate;
    SPDLOG_INFO("Orientation delegates: app {}, scene {} responds {}",
                appDelegate != nil ? class_getName([appDelegate class]) : "none",
                sceneDelegate != nil ? class_getName([sceneDelegate class]) : "none",
                [sceneDelegate respondsToSelector:@selector(supportedInterfaceOrientationsForWindowScene:)] ? "yes"
                                                                                                             : "no");
}

void InstallGeometryUpdateHook() {
#ifdef LUS_UIKIT_ORIENTATION_LOCK
    if (@available(iOS 26.0, *)) {
        Class sceneDelegate = [sWindow.windowScene.delegate class];
        if (sceneDelegate == Nil) {
            return;
        }
        Method update = class_getInstanceMethod(sceneDelegate, @selector(windowScene:didUpdateEffectiveGeometry:));
        if (update != nullptr) {
            sOriginalGeometryUpdate = method_setImplementation(update, (IMP)GeometryUpdateHook);
        } else {
            class_addMethod(sceneDelegate, @selector(windowScene:didUpdateEffectiveGeometry:),
                            (IMP)GeometryUpdateHook, "v@:@@");
        }
    }
#endif
}
} // namespace

void UIKitRequestOrientationLock(SDL_Window* window) {
    sWindow = WindowOf(window);
    UIViewController* controller = sWindow.rootViewController;
    if (controller == nil) {
        SPDLOG_WARN("No UIKit view controller; the orientation lock is not requested");
        return;
    }
    sWindow.backgroundColor = UIColor.blackColor;
    controller.view.backgroundColor = UIColor.blackColor;
#ifdef LUS_UIKIT_ORIENTATION_LOCK
    if (@available(iOS 26.0, *)) {
        Method transition = class_getInstanceMethod([controller class], @selector(viewWillTransitionToSize:
                                                                                         withTransitionCoordinator:));
        if (transition != nullptr) {
            sOriginalTransition = method_setImplementation(transition, (IMP)TransitionHook);
        }
        class_addMethod([controller class], @selector(prefersInterfaceOrientationLocked),
                        (IMP)PrefersInterfaceOrientationLocked, "B@:");
        Method supported = class_getInstanceMethod([controller class], @selector(supportedInterfaceOrientations));
        if (supported != nullptr) {
            sOriginalSupported = method_setImplementation(supported, (IMP)SupportedOrientationsHook);
        }
        LogDelegates();
        InstallGeometryUpdateHook();
        ApplyDisplayMode(SceneSize(), "launch", true);
        [UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
        [NSNotificationCenter.defaultCenter addObserverForName:UIDeviceOrientationDidChangeNotification
                                                        object:nil
                                                         queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification*) {
                                                        OnDeviceOrientation();
                                                    }];
    }
#endif
}

void UIKitLogOrientation(SDL_Window* window, int width, int height) {
    UIWindowScene* scene = WindowOf(window).windowScene;
    if (scene == nil) {
        return;
    }
    UIWindowSceneGeometry* geometry = scene.effectiveGeometry;
    const char* lock = "not available";
#ifdef LUS_UIKIT_ORIENTATION_LOCK
    if (@available(iOS 26.0, *)) {
        lock = geometry.isInterfaceOrientationLocked ? "held" : "not held";
    }
#endif
    SPDLOG_INFO("Window {}x{}: interface orientation {}, orientation lock {}", width, height,
                (long)geometry.interfaceOrientation, lock);
}

} // namespace Fast

#endif
