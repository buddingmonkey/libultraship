#include "fast/backends/gfx_uikit.h"

#if defined(__IOS__) && !defined(__VISIONOS__)

#import <UIKit/UIKit.h>
#include <objc/runtime.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <spdlog/spdlog.h>

namespace Fast {

namespace {
constexpr int64_t kRelockDelayNs = 1000 * NSEC_PER_MSEC;

BOOL sLockWanted = NO;
UIWindow* sWindow = nil;
IMP sOriginalTransition = nullptr;
uint64_t sTurn = 0;

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

void SetLock(BOOL wanted, const char* why) {
    if (@available(iOS 26.0, *)) {
        sLockWanted = wanted;
        [sWindow.rootViewController setNeedsUpdateOfPrefersInterfaceOrientationLocked];
        SPDLOG_INFO("Orientation lock {} ({}): interface orientation {}, device orientation {}",
                    wanted ? "requested" : "released", why, InterfaceOrientation(), DeviceOrientation());
    }
}

void SupportOnly(long orientation) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS,
                orientation == (long)UIInterfaceOrientationLandscapeLeft ? "LandscapeLeft" : "LandscapeRight");
    [sWindow.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
}

void TransitionHook(id self, SEL cmd, CGSize size, id<UIViewControllerTransitionCoordinator> coordinator) {
    SPDLOG_INFO("Rotation transition to {}x{}, animated {}, {:.2f} s: interface orientation {}, device orientation {}",
                (int)size.width, (int)size.height, coordinator.animated ? "yes" : "no", coordinator.transitionDuration,
                InterfaceOrientation(), DeviceOrientation());
    ((void (*)(id, SEL, CGSize, id))sOriginalTransition)(self, cmd, size, coordinator);
}

void OnDeviceOrientation() {
    const long device = DeviceOrientation();
    if (!IsLandscape(device)) {
        if (!sLockWanted) {
            sTurn++;
            SetLock(YES, "device left landscape");
        }
        return;
    }
    const uint64_t turn = ++sTurn;
    SetLock(NO, "device in landscape");
    SupportOnly(device);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRelockDelayNs), dispatch_get_main_queue(), ^{
        if (turn == sTurn) {
            SetLock(YES, "turn complete");
        }
    });
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
    if (@available(iOS 26.0, *)) {
        Method transition = class_getInstanceMethod([controller class], @selector(viewWillTransitionToSize:
                                                                                         withTransitionCoordinator:));
        if (transition != nullptr) {
            sOriginalTransition = method_setImplementation(transition, (IMP)TransitionHook);
        }
        class_addMethod([controller class], @selector(prefersInterfaceOrientationLocked),
                        (IMP)PrefersInterfaceOrientationLocked, "B@:");
        SupportOnly(InterfaceOrientation());
        SetLock(YES, "launch");
        [UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
        [NSNotificationCenter.defaultCenter addObserverForName:UIDeviceOrientationDidChangeNotification
                                                        object:nil
                                                         queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification*) {
                                                        OnDeviceOrientation();
                                                    }];
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
