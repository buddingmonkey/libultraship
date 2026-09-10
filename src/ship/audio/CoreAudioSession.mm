#include "CoreAudioSession.h"

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>
#include <spdlog/spdlog.h>
#include <mutex>

namespace Ship {

namespace {
std::mutex gHandlerMutex;
IOSAudioSessionHandlers gHandlers;
NSMutableArray* gObserverTokens = nil;

const char* ErrorText(NSError* error) {
    const char* text = error.localizedDescription.UTF8String;
    return text != nullptr ? text : "unknown error";
}

void HandleInterruptionNotification(NSNotification* notification) {
    NSNumber* rawType = notification.userInfo[AVAudioSessionInterruptionTypeKey];
    if (rawType == nil) {
        return;
    }
    const auto type = static_cast<AVAudioSessionInterruptionType>(rawType.unsignedIntegerValue);

    // Held across the handler call on purpose; this is what lets Stop() fence against it.
    std::lock_guard<std::mutex> guard(gHandlerMutex);

    if (type == AVAudioSessionInterruptionTypeBegan) {
        SPDLOG_INFO("CoreAudio: audio session interrupted");
        if (gHandlers.OnInterruptionBegan) {
            gHandlers.OnInterruptionBegan();
        }
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        NSNumber* rawOptions = notification.userInfo[AVAudioSessionInterruptionOptionKey];
        const bool shouldResume =
            rawOptions != nil &&
            (rawOptions.unsignedIntegerValue & AVAudioSessionInterruptionOptionShouldResume) != 0;

        SPDLOG_INFO("CoreAudio: audio session interruption ended (resume: {})", shouldResume);
        if (gHandlers.OnInterruptionEnded) {
            gHandlers.OnInterruptionEnded(shouldResume);
        }
    }
}

void HandleRouteChangeNotification(NSNotification* notification) {
    NSNumber* rawReason = notification.userInfo[AVAudioSessionRouteChangeReasonKey];
    if (rawReason == nil) {
        return;
    }
    const auto reason = static_cast<AVAudioSessionRouteChangeReason>(rawReason.unsignedIntegerValue);
    if (reason != AVAudioSessionRouteChangeReasonOldDeviceUnavailable) {
        return;
    }

    std::lock_guard<std::mutex> guard(gHandlerMutex);

    SPDLOG_INFO("CoreAudio: output route went away");
    if (gHandlers.OnOutputRouteLost) {
        gHandlers.OnOutputRouteLost();
    }
}

void HandleMediaServicesResetNotification(NSNotification* notification) {
    (void)notification;

    std::lock_guard<std::mutex> guard(gHandlerMutex);

    SPDLOG_WARN("CoreAudio: media services were reset, rebuilding audio");
    if (gHandlers.OnMediaServicesReset) {
        gHandlers.OnMediaServicesReset();
    }
}
} // namespace

bool ConfigureIOSAudioSession(double sampleRate) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;

    if (![session setCategory:AVAudioSessionCategoryPlayback error:&error]) {
        SPDLOG_ERROR("CoreAudio: Failed to set audio session category: {}", ErrorText(error));
        return false;
    }

    if (![session setPreferredSampleRate:sampleRate error:&error]) {
        SPDLOG_WARN("CoreAudio: Failed to set preferred sample rate {}: {}", sampleRate, ErrorText(error));
    }

    if (![session setActive:YES error:&error]) {
        SPDLOG_ERROR("CoreAudio: Failed to activate audio session: {}", ErrorText(error));
        return false;
    }

    return true;
}

bool ActivateIOSAudioSession() {
    NSError* error = nil;
    if (![[AVAudioSession sharedInstance] setActive:YES error:&error]) {
        SPDLOG_ERROR("CoreAudio: Failed to reactivate audio session: {}", ErrorText(error));
        return false;
    }
    return true;
}

void DeactivateIOSAudioSession() {
    NSError* error = nil;
    if (![[AVAudioSession sharedInstance] setActive:NO error:&error]) {
        SPDLOG_WARN("CoreAudio: Failed to deactivate audio session: {}", ErrorText(error));
    }
}

void StartIOSAudioSessionObservers(IOSAudioSessionHandlers handlers) {
    std::lock_guard<std::mutex> guard(gHandlerMutex);

    gHandlers = std::move(handlers);

    if (gObserverTokens != nil) {
        return;
    }

    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    AVAudioSession* session = [AVAudioSession sharedInstance];
    gObserverTokens = [NSMutableArray array];

    [gObserverTokens addObject:[center addObserverForName:AVAudioSessionInterruptionNotification
                                                   object:session
                                                    queue:nil
                                               usingBlock:^(NSNotification* notification) {
                                                   HandleInterruptionNotification(notification);
                                               }]];

    [gObserverTokens addObject:[center addObserverForName:AVAudioSessionRouteChangeNotification
                                                   object:session
                                                    queue:nil
                                               usingBlock:^(NSNotification* notification) {
                                                   HandleRouteChangeNotification(notification);
                                               }]];

    [gObserverTokens addObject:[center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                                                   object:nil
                                                    queue:nil
                                               usingBlock:^(NSNotification* notification) {
                                                   HandleMediaServicesResetNotification(notification);
                                               }]];
}

void StopIOSAudioSessionObservers() {
    std::lock_guard<std::mutex> guard(gHandlerMutex);

    gHandlers = IOSAudioSessionHandlers{};

    if (gObserverTokens != nil) {
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        for (id token in gObserverTokens) {
            [center removeObserver:token];
        }
        gObserverTokens = nil;
    }
}

} // namespace Ship

#endif
