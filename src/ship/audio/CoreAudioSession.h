#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

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
 * @brief Deactivates the process-wide AVAudioSession, releasing the audio route.
 */
void DeactivateIOSAudioSession();

} // namespace Ship

#endif
