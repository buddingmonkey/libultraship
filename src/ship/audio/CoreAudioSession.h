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
 * iOS routes every app's audio through an AVAudioSession. An app that never configures one
 * gets the default SoloAmbient category, which is silenced by the ring/silent switch and
 * stops on screen lock, so the RemoteIO unit would run but produce no audible output.
 *
 * Implemented in CoreAudioSession.mm because AVAudioSession is Objective-C only. macOS has
 * no equivalent session and does not need this.
 *
 * @param sampleRate Preferred hardware sample rate in Hz. Advisory only -- iOS picks the
 *                   nearest rate the hardware supports and the RemoteIO unit resamples if
 *                   the two differ.
 * @return true if the session was configured and activated.
 */
bool ConfigureIOSAudioSession(double sampleRate);

/**
 * @brief Reactivates the process-wide AVAudioSession without reconfiguring it.
 *
 * Used when resuming after an interruption; the category set by
 * ConfigureIOSAudioSession() survives the interruption, only the activation is lost.
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
    /**
     * The audio route has been taken away -- a phone call, Siri, an alarm. iOS has already
     * stopped the unit; nothing restarts it automatically.
     */
    std::function<void()> OnInterruptionBegan;

    /**
     * The interruption is over. The argument is the system's ShouldResume hint, false when
     * iOS expects the user to restart playback themselves.
     */
    std::function<void(bool shouldResume)> OnInterruptionEnded;

    /**
     * The previous output route disappeared -- headphones unplugged, Bluetooth dropped. iOS
     * pauses the unit in this case, deliberately, so that yanking headphones does not blast
     * audio out of the speaker. Restarting it is the player's decision.
     */
    std::function<void()> OnOutputRouteLost;

    /**
     * The media server died and restarted. Every Core Audio object the process holds is now
     * invalid -- the unit cannot be restarted, it has to be disposed and rebuilt, and the
     * session reconfigured from scratch. Rare, but it strands the game in silence when it
     * happens.
     */
    std::function<void()> OnMediaServicesReset;
};

/**
 * @brief Observes the AVAudioSession notifications described by IOSAudioSessionHandlers.
 *
 * Lifetime contract: the handlers run while an internal lock is held, and
 * StopIOSAudioSessionObservers() takes that same lock and clears them. It therefore blocks
 * until any in-flight handler returns and guarantees none starts afterwards, which is what
 * makes it safe for a handler to capture a raw `this`. Call Stop before tearing down
 * anything the handlers touch.
 *
 * A handler must not call Start or Stop itself -- the lock is not recursive.
 *
 * Calling Start twice replaces the handlers rather than stacking observers.
 */
void StartIOSAudioSessionObservers(IOSAudioSessionHandlers handlers);

/**
 * @brief Removes the observers and clears the handlers.
 *
 * Safe to call when nothing is registered.
 */
void StopIOSAudioSessionObservers();

} // namespace Ship

#endif
