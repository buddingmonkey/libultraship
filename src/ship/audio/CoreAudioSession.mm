#include "CoreAudioSession.h"

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>
#include <spdlog/spdlog.h>
#include <mutex>

namespace Ship {

namespace {
/**
 * Guards the handlers below. Deliberately held while a handler runs so that
 * StopIOSAudioInterruptionObserver() cannot return until the handler has finished -- see
 * the lifetime contract in CoreAudioSession.h.
 */
std::mutex gHandlerMutex;
std::function<void()> gOnInterruptionBegan;
std::function<void(bool)> gOnInterruptionEnded;
id gInterruptionObserver = nil;

/**
 * @brief Returns a loggable string for an NSError, tolerating a nil error.
 *
 * Messaging nil yields nil, so a failing API that neglects to populate the out-error would
 * otherwise hand spdlog a null char pointer.
 */
const char* ErrorText(NSError* error) {
    const char* text = error.localizedDescription.UTF8String;
    return text != nullptr ? text : "unknown error";
}

/**
 * Dispatches an AVAudioSessionInterruptionNotification to the registered handlers.
 */
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
        if (gOnInterruptionBegan) {
            gOnInterruptionBegan();
        }
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        NSNumber* rawOptions = notification.userInfo[AVAudioSessionInterruptionOptionKey];
        const bool shouldResume =
            rawOptions != nil &&
            (rawOptions.unsignedIntegerValue & AVAudioSessionInterruptionOptionShouldResume) != 0;

        SPDLOG_INFO("CoreAudio: audio session interruption ended (resume: {})", shouldResume);
        if (gOnInterruptionEnded) {
            gOnInterruptionEnded(shouldResume);
        }
    }
}
} // namespace

bool ConfigureIOSAudioSession(double sampleRate) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;

    // Playback keeps game audio running with the ring/silent switch engaged, matching how
    // the desktop ports behave. It does not mix with other apps' audio.
    if (![session setCategory:AVAudioSessionCategoryPlayback error:&error]) {
        SPDLOG_ERROR("CoreAudio: Failed to set audio session category: {}", ErrorText(error));
        return false;
    }

    if (![session setPreferredSampleRate:sampleRate error:&error]) {
        // Not fatal -- the unit resamples to whatever rate the session settles on.
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

void StartIOSAudioInterruptionObserver(std::function<void()> onInterruptionBegan,
                                       std::function<void(bool shouldResume)> onInterruptionEnded) {
    std::lock_guard<std::mutex> guard(gHandlerMutex);

    gOnInterruptionBegan = std::move(onInterruptionBegan);
    gOnInterruptionEnded = std::move(onInterruptionEnded);

    if (gInterruptionObserver != nil) {
        return;
    }

    gInterruptionObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVAudioSessionInterruptionNotification
                    object:[AVAudioSession sharedInstance]
                     queue:nil
                usingBlock:^(NSNotification* notification) {
                    HandleInterruptionNotification(notification);
                }];
}

void StopIOSAudioInterruptionObserver() {
    std::lock_guard<std::mutex> guard(gHandlerMutex);

    // Clearing the handlers is the load-bearing part: a notification block that was already
    // in flight when the observer was removed still runs, and finds nothing to call.
    gOnInterruptionBegan = nullptr;
    gOnInterruptionEnded = nullptr;

    if (gInterruptionObserver != nil) {
        [[NSNotificationCenter defaultCenter] removeObserver:gInterruptionObserver];
        gInterruptionObserver = nil;
    }
}

} // namespace Ship

#endif
