#pragma once

#ifdef __APPLE__

#include "AudioPlayer.h"
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <mutex>
#include <pthread.h>

namespace Ship {
/**
 * @brief AudioPlayer implementation backed by Apple's Core Audio framework.
 *
 * CoreAudioAudioPlayer uses an output AudioUnit with a render callback to pull
 * interleaved PCM samples from an internal ring buffer. DoPlay() pushes data into
 * the ring buffer, and the Core Audio render callback reads from it on the audio
 * thread.
 *
 * The unit is kAudioUnitSubType_HALOutput on macOS and kAudioUnitSubType_RemoteIO on
 * iOS, which additionally requires an active AVAudioSession (see CoreAudioSession.h).
 *
 * This backend is only available on Apple platforms (macOS / iOS).
 */
class CoreAudioAudioPlayer : public AudioPlayer {
  public:
    /**
     * @brief Constructs a CoreAudioAudioPlayer with the given audio settings.
     * @param settings Sample rate, buffer size, desired buffered frames, and channel mode.
     */
    CoreAudioAudioPlayer(AudioSettings settings);
    ~CoreAudioAudioPlayer();

    /**
     * @brief Returns the number of audio frames currently queued in the ring buffer.
     */
    int Buffered() override;

#if TARGET_OS_IPHONE
    /**
     * @brief Stops the output unit and hands the audio session back to the system.
     *
     * iOS deactivates a suspended app's session anyway, but doing it here is what lets
     * whatever the game interrupted resume while the game is off screen.
     */
    void Suspend() override;

    /**
     * @brief Reactivates the audio session and starts the output unit again.
     *
     * Nothing does this for us: the notification that once reported an app being
     * suspended with an active session was deprecated in iOS 14.5 and is no longer sent.
     */
    void Resume() override;
#endif

  protected:
    /**
     * @brief Opens and configures the Core Audio output unit.
     * @return true if the audio unit was created and started successfully.
     */
    bool DoInit() override;

    /**
     * @brief Stops and disposes of the Core Audio output unit and frees the ring buffer.
     */
    void DoClose() override;

    /**
     * @brief Copies interleaved PCM samples into the ring buffer for playback.
     * @param buf Interleaved sample data.
     * @param len Length of @p buf in bytes.
     */
    void DoPlay(const uint8_t* buf, size_t len) override;

  private:
    /**
     * @brief Core Audio render callback that pulls samples from the ring buffer.
     *
     * Called on the audio thread by the output AudioUnit whenever it needs more
     * sample data. Reads from the ring buffer under the mutex lock.
     *
     * @param inRefCon Pointer to the owning CoreAudioAudioPlayer instance.
     * @param ioActionFlags Render action flags (unused).
     * @param inTimeStamp Timestamp for the requested render (unused).
     * @param inBusNumber Output bus number (unused).
     * @param inNumberFrames Number of frames requested.
     * @param ioData Buffer list to fill with audio data.
     * @return noErr on success.
     */
    static OSStatus CoreAudioRenderCallback(void* inRefCon, AudioUnitRenderActionFlags* ioActionFlags,
                                            const AudioTimeStamp* inTimeStamp, UInt32 inBusNumber,
                                            UInt32 inNumberFrames, AudioBufferList* ioData);

    /**
     * @brief Allocates the ring buffer and starts an output unit for @p numChannels.
     *
     * Cleans up after itself on failure, so it can be called again with a different
     * channel count.
     *
     * @param numChannels Interleaved output channel count to request from the device.
     * @return true if the unit was configured and started.
     */
    bool OpenOutputUnit(int32_t numChannels);

    /**
     * @brief Stops and disposes the output unit and releases the ring buffer.
     *
     * Safe to call when nothing is open.
     */
    void CloseOutputUnit();

#if TARGET_OS_IPHONE
    /**
     * @brief Reactivates the audio session and starts the existing unit again.
     *
     * For the cases where the unit survives and was merely paused -- an interruption ending,
     * an output route disappearing. A media services reset is not one of these: the unit is
     * dead and must be rebuilt.
     *
     * @param reason Short description used in the failure log.
     */
    void RestartOutputUnit(const char* reason);

    /**
     * @brief Serialises the calls that start and stop the unit.
     *
     * Suspend(), Resume() and RestartOutputUnit() can be reached from the app's main
     * thread and from a session notification at the same time, and the AudioUnit calls
     * they make are not thread safe. Deliberately not taken in DoClose(): a handler holds
     * the session lock while it waits for this one, so taking them in the other order
     * anywhere would deadlock. Distinct from mMutex, which the render callback holds --
     * AudioOutputUnitStop() waits for that callback to return.
     */
    std::mutex mUnitMutex;
#endif

    AudioUnit mAudioUnit = nullptr;
    int32_t mNumChannels = 0;
    uint8_t* mRingBuffer = nullptr; ///< Lock-protected circular buffer for audio samples.
    size_t mRingBufferSize = 0;     ///< Total size of the ring buffer in bytes.
    size_t mRingBufferReadPos = 0;  ///< Current read position in the ring buffer.
    size_t mRingBufferWritePos = 0; ///< Current write position in the ring buffer.
    pthread_mutex_t mMutex;         ///< Guards concurrent access to the ring buffer.
    bool mInitialized;
};
} // namespace Ship
#endif
