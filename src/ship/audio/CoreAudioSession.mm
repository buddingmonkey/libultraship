#include "CoreAudioSession.h"

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>
#include <spdlog/spdlog.h>

namespace Ship {

namespace {
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

void DeactivateIOSAudioSession() {
    NSError* error = nil;
    if (![[AVAudioSession sharedInstance] setActive:NO error:&error]) {
        SPDLOG_WARN("CoreAudio: Failed to deactivate audio session: {}", ErrorText(error));
    }
}

} // namespace Ship

#endif
