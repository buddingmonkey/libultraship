#ifdef __APPLE__
#include "ship/audio/CoreAudioAudioPlayer.h"
#include "CoreAudioSession.h"
#include <TargetConditionals.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace Ship {

CoreAudioAudioPlayer::CoreAudioAudioPlayer(AudioSettings settings) : AudioPlayer(settings), mInitialized(false) {
    pthread_mutex_init(&mMutex, NULL);
}

CoreAudioAudioPlayer::~CoreAudioAudioPlayer() {
    SPDLOG_TRACE("destruct CoreAudio audio player");
    DoClose();
    pthread_mutex_destroy(&mMutex);
}

void CoreAudioAudioPlayer::DoClose() {
#if TARGET_OS_IPHONE
    // Must come first: it fences against an interruption handler that is mid-flight and
    // about to touch mAudioUnit. See the lifetime contract in CoreAudioSession.h.
    StopIOSAudioInterruptionObserver();
#endif

    const bool wasOpen = mAudioUnit != nullptr;

    CloseOutputUnit();

#if TARGET_OS_IPHONE
    if (wasOpen) {
        // Hand the audio route back once the unit is gone, so other apps can take over.
        DeactivateIOSAudioSession();
    }
#endif
}

void CoreAudioAudioPlayer::CloseOutputUnit() {
    if (mAudioUnit != nullptr) {
        AudioOutputUnitStop(mAudioUnit);
        AudioUnitUninitialize(mAudioUnit);
        AudioComponentInstanceDispose(mAudioUnit);
        mAudioUnit = nullptr;
    }

    mInitialized = false;

    if (mRingBuffer) {
        delete[] mRingBuffer;
        mRingBuffer = nullptr;
    }
}

bool CoreAudioAudioPlayer::DoInit() {
#if TARGET_OS_IPHONE
    // Nothing plays on iOS until the process has an active audio session.
    if (!ConfigureIOSAudioSession(this->GetSampleRate())) {
        return false;
    }
#endif

    const int32_t requestedChannels = this->GetNumOutputChannels();

    if (!OpenOutputUnit(requestedChannels)) {
        if (requestedChannels == 2) {
            return false;
        }

        // Surround was refused. Every iOS output route is stereo, as are plenty of macOS
        // devices, so retry rather than leaving the game silent. DowngradeAudioChannels()
        // keeps Play() from matrix decoding into a unit that is now only two channels wide.
        SPDLOG_WARN("CoreAudio: {} channel output unavailable, retrying in stereo", requestedChannels);
        this->DowngradeAudioChannels(AudioChannelsSetting::audioStereo);

        if (!OpenOutputUnit(2)) {
            return false;
        }
    }

#if TARGET_OS_IPHONE
    // A phone call or Siri stops the unit and iOS never restarts it for us.
    StartIOSAudioInterruptionObserver(
        [this]() {
            if (mAudioUnit != nullptr) {
                AudioOutputUnitStop(mAudioUnit);
            }
        },
        [this](bool shouldResume) {
            if (!shouldResume) {
                // iOS withholds ShouldResume when it expects the user to hit play, but a
                // game has no such control and would then stay silent for good. Try anyway:
                // if another app really does hold the route, reactivation just fails.
                SPDLOG_INFO("CoreAudio: resuming without a ShouldResume hint");
            }

            if (!ActivateIOSAudioSession() || mAudioUnit == nullptr) {
                return;
            }

            const OSStatus status = AudioOutputUnitStart(mAudioUnit);
            if (status != noErr) {
                SPDLOG_ERROR("CoreAudio: Failed to restart audio unit after interruption: {}", status);
            }
        });
#endif

    return true;
}

bool CoreAudioAudioPlayer::OpenOutputUnit(int32_t numChannels) {
    OSStatus status;

    mNumChannels = numChannels;

    const size_t bytesPerSample = sizeof(int16_t);
    const size_t bytesPerFrame = bytesPerSample * mNumChannels;

    mRingBufferSize = 6000 * bytesPerFrame;
    mRingBuffer = new uint8_t[mRingBufferSize];
    mRingBufferReadPos = 0;
    mRingBufferWritePos = 0;

    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
    // iOS has no HAL. RemoteIO is the hardware I/O unit; element 0 is the output bus.
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
    desc.componentSubType = kAudioUnitSubType_HALOutput;
#endif
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    if (component == NULL) {
        SPDLOG_ERROR("CoreAudio: Failed to find audio component");
        CloseOutputUnit();
        return false;
    }

    status = AudioComponentInstanceNew(component, &mAudioUnit);
    if (status != noErr) {
        SPDLOG_ERROR("CoreAudio: Failed to create audio component instance: {}", status);
        CloseOutputUnit();
        return false;
    }

    UInt32 flag = 1;
    status = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &flag,
                                  sizeof(flag));
    if (status != noErr) {
        SPDLOG_ERROR("CoreAudio: Failed to enable output: {}", status);
        CloseOutputUnit();
        return false;
    }

    AudioStreamBasicDescription format;
    format.mSampleRate = this->GetSampleRate();
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = bytesPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = bytesPerFrame;
    format.mChannelsPerFrame = mNumChannels;
    format.mBitsPerChannel = 16;

    status = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                  sizeof(format));
    if (status != noErr) {
        SPDLOG_ERROR("CoreAudio: Failed to set stream format: {}", status);
        CloseOutputUnit();
        return false;
    }

    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc = CoreAudioRenderCallback;
    callbackStruct.inputProcRefCon = this;

    status = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                  &callbackStruct, sizeof(callbackStruct));
    if (status != noErr) {
        SPDLOG_ERROR("CoreAudio: Failed to set render callback: {}", status);
        CloseOutputUnit();
        return false;
    }

    status = AudioUnitInitialize(mAudioUnit);
    if (status != noErr) {
        SPDLOG_ERROR("CoreAudio: Failed to initialize audio unit: {}", status);
        CloseOutputUnit();
        return false;
    }

    status = AudioOutputUnitStart(mAudioUnit);
    if (status != noErr) {
        SPDLOG_ERROR("CoreAudio: Failed to start audio unit: {}", status);
        CloseOutputUnit();
        return false;
    }

    mInitialized = true;
    return true;
}

int CoreAudioAudioPlayer::Buffered() {
    pthread_mutex_lock(&mMutex);
    size_t buffered;

    if (mRingBufferWritePos >= mRingBufferReadPos) {
        buffered = mRingBufferWritePos - mRingBufferReadPos;
    } else {
        buffered = mRingBufferSize - (mRingBufferReadPos - mRingBufferWritePos);
    }

    const size_t bytesPerFrame = sizeof(int16_t) * mNumChannels;
    int samples = buffered / bytesPerFrame;

    pthread_mutex_unlock(&mMutex);
    return samples;
}

void CoreAudioAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    pthread_mutex_lock(&mMutex);

    const size_t bytesPerFrame = sizeof(int16_t) * mNumChannels;
    const size_t maxBuffered = 6000 * bytesPerFrame;

    size_t available;
    if (mRingBufferWritePos >= mRingBufferReadPos) {
        available = mRingBufferSize - (mRingBufferWritePos - mRingBufferReadPos);
    } else {
        available = mRingBufferReadPos - mRingBufferWritePos;
    }

    if (available >= len) {
        size_t writeEnd = mRingBufferWritePos + len;

        if (writeEnd <= mRingBufferSize) {
            memcpy(mRingBuffer + mRingBufferWritePos, buf, len);
        } else {
            size_t firstChunk = mRingBufferSize - mRingBufferWritePos;
            memcpy(mRingBuffer + mRingBufferWritePos, buf, firstChunk);
            memcpy(mRingBuffer, buf + firstChunk, len - firstChunk);
        }

        mRingBufferWritePos = (mRingBufferWritePos + len) % mRingBufferSize;
    }

    pthread_mutex_unlock(&mMutex);
}

OSStatus CoreAudioAudioPlayer::CoreAudioRenderCallback(void* inRefCon, AudioUnitRenderActionFlags* ioActionFlags,
                                                       const AudioTimeStamp* inTimeStamp, UInt32 inBusNumber,
                                                       UInt32 inNumberFrames, AudioBufferList* ioData) {
    CoreAudioAudioPlayer* player = static_cast<CoreAudioAudioPlayer*>(inRefCon);

    for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
        AudioBuffer* buffer = &ioData->mBuffers[i];
        UInt8* outputBuffer = static_cast<UInt8*>(buffer->mData);
        UInt32 bytesToWrite = buffer->mDataByteSize;

        pthread_mutex_lock(&player->mMutex);

        size_t available;
        if (player->mRingBufferWritePos >= player->mRingBufferReadPos) {
            available = player->mRingBufferWritePos - player->mRingBufferReadPos;
        } else {
            available = player->mRingBufferSize - (player->mRingBufferReadPos - player->mRingBufferWritePos);
        }

        UInt32 bytesToCopy = bytesToWrite;
        if (bytesToCopy > available) {
            bytesToCopy = available;
        }

        if (bytesToCopy > 0) {
            size_t readEnd = player->mRingBufferReadPos + bytesToCopy;

            if (readEnd <= player->mRingBufferSize) {
                memcpy(outputBuffer, player->mRingBuffer + player->mRingBufferReadPos, bytesToCopy);
            } else {
                size_t firstChunk = player->mRingBufferSize - player->mRingBufferReadPos;
                memcpy(outputBuffer, player->mRingBuffer + player->mRingBufferReadPos, firstChunk);
                memcpy(outputBuffer + firstChunk, player->mRingBuffer, bytesToCopy - firstChunk);
            }

            player->mRingBufferReadPos = (player->mRingBufferReadPos + bytesToCopy) % player->mRingBufferSize;
        }

        if (bytesToCopy < bytesToWrite) {
            memset(outputBuffer + bytesToCopy, 0, bytesToWrite - bytesToCopy);
        }

        pthread_mutex_unlock(&player->mMutex);
    }

    return noErr;
}

} // namespace Ship
#endif
