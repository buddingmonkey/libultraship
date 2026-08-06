#pragma once

#ifdef __APPLE__

#include "AudioPlayer.h"
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <mutex>
#include <pthread.h>

namespace Ship {
/** @brief AudioPlayer backed by Core Audio: a HALOutput (macOS) or RemoteIO (iOS) unit fed by a ring buffer. */
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
    /** @brief Stops the unit and hands the session back, so another app can take the route. */
    void Suspend() override;

    /** @brief Reactivates the session and restarts the unit; iOS no longer signals this itself. */
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
     * @brief Allocates the ring buffer and starts an output unit; cleans up after itself on failure.
     * @param numChannels Interleaved output channel count to request from the device.
     * @return true if the unit was configured and started.
     */
    bool OpenOutputUnit(int32_t numChannels);

    /** @brief Stops and disposes the output unit and releases the ring buffer; safe when nothing is open. */
    void CloseOutputUnit();

#if TARGET_OS_IPHONE
    /**
     * @brief Reactivates the session and restarts the existing unit, for cases where it was merely paused.
     * @param reason Short description used in the failure log.
     */
    void RestartOutputUnit(const char* reason);

    /**
     * @brief Serialises the not-thread-safe calls that start and stop the unit, reachable from the
     * main thread and a session notification at once. Not taken in DoClose(), which a handler waits
     * behind through the session lock -- the reverse order deadlocks. Distinct from mMutex.
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
