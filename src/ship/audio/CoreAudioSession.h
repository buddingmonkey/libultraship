#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <functional>

namespace Ship {

/** @brief Configures and activates the process-wide AVAudioSession; the default category plays silent. */
bool ConfigureIOSAudioSession(double sampleRate);

/** @brief Reactivates the session after an interruption; the configured category survives it. */
bool ActivateIOSAudioSession();

/** @brief Deactivates the process-wide AVAudioSession, releasing the audio route. */
void DeactivateIOSAudioSession();

/** @brief Callbacks for the AVAudioSession events that require the player to act; any may be left empty. */
struct IOSAudioSessionHandlers {
    /** Route taken away by a call, Siri or an alarm; iOS stopped the unit and nothing restarts it. */
    std::function<void()> OnInterruptionBegan;

    /** The interruption is over; the argument is the system's ShouldResume hint. */
    std::function<void(bool shouldResume)> OnInterruptionEnded;

    /** The output route disappeared -- headphones, Bluetooth. iOS pauses the unit; restarting is ours. */
    std::function<void()> OnOutputRouteLost;

    /** The media server restarted; every Core Audio object is dead and must be rebuilt. */
    std::function<void()> OnMediaServicesReset;
};

/** @brief Observes the session notifications; Stop() fences against in-flight handlers, so raw `this` is safe. */
void StartIOSAudioSessionObservers(IOSAudioSessionHandlers handlers);

/** @brief Removes the observers and clears the handlers. Not callable from a handler; the lock is not recursive. */
void StopIOSAudioSessionObservers();

} // namespace Ship

#endif
