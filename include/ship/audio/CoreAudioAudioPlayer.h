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
    /** @brief Stops the output unit and hands the audio session back to the system. */
    void Suspend() override;

    /** @brief Reactivates the audio session and starts the output unit again. */
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
     * @param numChannels Interleaved output channel count to request from the device.
     * @return true if the unit was configured and started.
     */
    bool OpenOutputUnit(int32_t numChannels);

    /** @brief Stops and disposes the output unit and releases the ring buffer. */
    void CloseOutputUnit();

#if TARGET_OS_IPHONE
    /**
     * @brief Reactivates the audio session and starts the existing unit again.
     *
     * @param reason Short description used in the failure log.
     */
    void RestartOutputUnit(const char* reason);

    /**
     * @brief Serialises the calls that start and stop the unit.
     *
     * Deliberately not taken in DoClose(): a session handler holds the session lock while it waits
     * for this one, so taking them in the other order anywhere would deadlock. Distinct from mMutex,
     * which the render callback holds; take this one first and never the other way round.
     */
    std::mutex mUnitMutex;

    /** @brief True between Suspend() and Resume(); blocks every restart path. Guarded by mUnitMutex. */
    bool mSuspended = false;
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
