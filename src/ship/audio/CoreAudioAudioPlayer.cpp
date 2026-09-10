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
    StopIOSAudioSessionObservers();
#endif

    const bool wasOpen = mAudioUnit != nullptr;

    CloseOutputUnit();

#if TARGET_OS_IPHONE
    if (wasOpen) {
        DeactivateIOSAudioSession();
    }
#endif
}

void CoreAudioAudioPlayer::CloseOutputUnit() {
    // Not under mMutex: AudioOutputUnitStop() waits for an in-flight render callback, which waits for it.
    if (mAudioUnit != nullptr) {
        AudioOutputUnitStop(mAudioUnit);
        AudioUnitUninitialize(mAudioUnit);
        AudioComponentInstanceDispose(mAudioUnit);
        mAudioUnit = nullptr;
    }

    mInitialized = false;

    pthread_mutex_lock(&mMutex);
    delete[] mRingBuffer;
    mRingBuffer = nullptr;
    mRingBufferSize = 0;
    mRingBufferReadPos = 0;
    mRingBufferWritePos = 0;
    pthread_mutex_unlock(&mMutex);
}

bool CoreAudioAudioPlayer::DoInit() {
#if TARGET_OS_IPHONE
    if (!ConfigureIOSAudioSession(this->GetSampleRate())) {
        return false;
    }
#endif

    const int32_t requestedChannels = this->GetNumOutputChannels();

    if (!OpenOutputUnit(requestedChannels)) {
        if (requestedChannels == 2) {
            return false;
        }

        SPDLOG_WARN("CoreAudio: {} channel output unavailable, retrying in stereo", requestedChannels);
        this->DowngradeAudioChannels(AudioChannelsSetting::audioStereo);

        if (!OpenOutputUnit(2)) {
            return false;
        }
    }

#if TARGET_OS_IPHONE
    IOSAudioSessionHandlers handlers;

    handlers.OnInterruptionBegan = [this]() {
        std::lock_guard<std::mutex> guard(mUnitMutex);
        if (mAudioUnit != nullptr) {
            AudioOutputUnitStop(mAudioUnit);
        }
    };

    handlers.OnInterruptionEnded = [this](bool shouldResume) {
        if (!shouldResume) {
            SPDLOG_INFO("CoreAudio: resuming without a ShouldResume hint");
        }
        RestartOutputUnit("interruption");
    };

    handlers.OnOutputRouteLost = [this]() { RestartOutputUnit("route change"); };

    handlers.OnMediaServicesReset = [this]() {
        std::lock_guard<std::mutex> guard(mUnitMutex);

        const int32_t channels = mNumChannels;

        CloseOutputUnit();

        if (!ConfigureIOSAudioSession(this->GetSampleRate())) {
            SPDLOG_ERROR("CoreAudio: Failed to reconfigure session after media services reset");
            return;
        }
        if (!OpenOutputUnit(channels)) {
            SPDLOG_ERROR("CoreAudio: Failed to rebuild audio unit after media services reset");
            return;
        }

        if (mSuspended) {
            AudioOutputUnitStop(mAudioUnit);
            DeactivateIOSAudioSession();
        }

        SPDLOG_INFO("CoreAudio: audio rebuilt after media services reset");
    };

    StartIOSAudioSessionObservers(std::move(handlers));
#endif

    return true;
}

#if TARGET_OS_IPHONE
void CoreAudioAudioPlayer::Suspend() {
    std::lock_guard<std::mutex> guard(mUnitMutex);

    mSuspended = true;

    if (mAudioUnit != nullptr) {
        AudioOutputUnitStop(mAudioUnit);
    }

    DeactivateIOSAudioSession();
}

void CoreAudioAudioPlayer::Resume() {
    {
        std::lock_guard<std::mutex> guard(mUnitMutex);
        mSuspended = false;
    }

    RestartOutputUnit("foreground");
}

void CoreAudioAudioPlayer::RestartOutputUnit(const char* reason) {
    std::lock_guard<std::mutex> guard(mUnitMutex);

    if (mSuspended || !ActivateIOSAudioSession() || mAudioUnit == nullptr) {
        return;
    }

    const OSStatus status = AudioOutputUnitStart(mAudioUnit);
    if (status != noErr) {
        SPDLOG_ERROR("CoreAudio: Failed to restart audio unit after {}: {}", reason, status);
    }
}
#endif

bool CoreAudioAudioPlayer::OpenOutputUnit(int32_t numChannels) {
    OSStatus status;

    const size_t bytesPerSample = sizeof(int16_t);
    const size_t bytesPerFrame = bytesPerSample * numChannels;

    pthread_mutex_lock(&mMutex);
    mNumChannels = numChannels;
    mRingBufferSize = 6000 * bytesPerFrame;
    mRingBuffer = new uint8_t[mRingBufferSize];
    mRingBufferReadPos = 0;
    mRingBufferWritePos = 0;
    pthread_mutex_unlock(&mMutex);

    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
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
    format.mChannelsPerFrame = numChannels;
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

    if (mRingBuffer == nullptr) {
        pthread_mutex_unlock(&mMutex);
        return 0;
    }

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

    if (mRingBuffer == nullptr) {
        pthread_mutex_unlock(&mMutex);
        return;
    }

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
