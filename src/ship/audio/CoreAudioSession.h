#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <functional>

namespace Ship {

/**
 * @brief Configures and activates the process-wide AVAudioSession for playback.
 *
 * Implemented in CoreAudioSession.mm because AVAudioSession is Objective-C only. macOS has no
 * equivalent session and does not need this.
 *
 * @param sampleRate Preferred hardware sample rate in Hz. Advisory only.
 * @return true if the session was configured and activated.
 */
bool ConfigureIOSAudioSession(double sampleRate);

/**
 * @brief Reactivates the process-wide AVAudioSession without reconfiguring it.
 *
 * @return true if the session was reactivated.
 */
bool ActivateIOSAudioSession();

/**
 * @brief Deactivates the process-wide AVAudioSession, releasing the audio route.
 */
void DeactivateIOSAudioSession();

/**
 * @brief Callbacks for the AVAudioSession events that require the player to act.
 *
 * Any member may be left empty; it is simply not called.
 */
struct IOSAudioSessionHandlers {
    /** The audio route has been taken away. iOS has already stopped the unit. */
    std::function<void()> OnInterruptionBegan;

    /** The interruption is over. The argument is the system's ShouldResume hint. */
    std::function<void(bool shouldResume)> OnInterruptionEnded;

    /** The previous output route disappeared. iOS pauses the unit rather than redirecting it. */
    std::function<void()> OnOutputRouteLost;

    /** The media server died and restarted; every Core Audio object the process holds is invalid. */
    std::function<void()> OnMediaServicesReset;
};

/**
 * @brief Observes the AVAudioSession notifications described by IOSAudioSessionHandlers.
 *
 * Lifetime contract: the handlers run while an internal lock is held, and
 * StopIOSAudioSessionObservers() takes that same lock and clears them, so it blocks until any
 * in-flight handler returns and none starts afterwards. A handler must not call Start or Stop
 * itself; the lock is not recursive. Calling Start twice replaces the handlers.
 */
void StartIOSAudioSessionObservers(IOSAudioSessionHandlers handlers);

/** @brief Removes the observers and clears the handlers. Safe to call when nothing is registered. */
void StopIOSAudioSessionObservers();

} // namespace Ship

#endif
