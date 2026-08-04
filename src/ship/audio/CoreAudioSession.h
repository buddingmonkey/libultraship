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
 * @brief Observes AVAudioSessionInterruptionNotification and reports it to the player.
 *
 * iOS silently stops the audio unit when something else takes the audio route -- a phone
 * call, Siri, an alarm. The unit is not restarted automatically, so without this the game
 * stays permanently silent after the first interruption.
 *
 * Lifetime contract: the handlers run while an internal lock is held, and
 * StopIOSAudioInterruptionObserver() takes that same lock and clears them. It therefore
 * blocks until any in-flight handler returns and guarantees none starts afterwards, which
 * is what makes it safe for a handler to capture a raw `this`. Call Stop before tearing
 * down anything the handlers touch.
 *
 * Calling Start twice replaces the handlers rather than stacking observers.
 *
 * @param onInterruptionBegan Invoked when the audio route has been taken away.
 * @param onInterruptionEnded Invoked when the interruption is over. The argument is the
 *                            system's ShouldResume hint; it is false when the user is
 *                            expected to restart playback themselves.
 */
void StartIOSAudioInterruptionObserver(std::function<void()> onInterruptionBegan,
                                       std::function<void(bool shouldResume)> onInterruptionEnded);

/**
 * @brief Removes the interruption observer and clears the handlers.
 *
 * Safe to call when no observer is registered.
 */
void StopIOSAudioInterruptionObserver();

} // namespace Ship

#endif
