// The macOS spatial path: AVSampleBufferAudioRenderer, fed from the ring.
//
// Why this exists rather than a flag on the miniaudio device: macOS spatializes
// multichannel audio -- the Fixed and Head Tracked modes Control Center offers
// for AirPods, and the built-in speakers' virtual surround -- only for audio
// that reaches it through AVFoundation with its channel layout attached. A HAL
// client is handed the device's own channel count, so miniaudio opened AirPods
// at two channels and folded 7.1 into them itself, and Control Center said
// "Not Available". Chromium met the same wall and answered it the same way.
//
// Not a real-time path. The renderer takes whole sample buffers and pulls
// nothing, so a timer on a serial queue keeps it topped up for as long as it
// says it will take more. How much that is is the renderer's call, and it is a
// lot: with AirPods it stops asking at a little over a second ahead of its
// clock, calls itself ready to start at about one, and starved in audible
// pulses when this held it to 100 ms. Everything else here follows from that
// second -- see SpatialStream.hpp.
//
// It is also particular about what it is given. Buffers of a few dozen frames,
// which is what a small ring hands over when it is read faster than it fills,
// sputtered at every start; whole buffers of 10 ms did not. See pump().
//
// The clock is a small state machine. Priming, it stands still at the frame
// the next buffer starts on while buffers are queued; once the renderer has
// enough, it runs from exactly there. A seek (the ring's flush), an automatic
// flush on a route change, and the queue running dry all go back to priming, so
// the renderer is never handed a buffer its clock has already passed -- which
// it would drop, and which is what a pulse is.

#include "SpatialStream.hpp"

#include "xpcog/core/AudioFormat.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <vector>

namespace xpcog {
namespace detail {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kTickSeconds = 0.010;
/// How long priming waits for more audio before starting on what it has. The
/// end of a track after a seek near its end never gets to "enough".
constexpr double kPrimeGiveUpSeconds = 0.200;

[[nodiscard]] AudioObjectID resolveDevice(const std::string& uid) {
    AudioObjectID device = kAudioObjectUnknown;
    UInt32        size   = sizeof(device);

    if (uid.empty()) {
        const AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                       &size, &device) != noErr) {
            return kAudioObjectUnknown;
        }
        return device;
    }

    CFStringRef cfUid = CFStringCreateWithCString(kCFAllocatorDefault, uid.c_str(),
                                                  kCFStringEncodingUTF8);
    if (cfUid == nullptr) {
        return kAudioObjectUnknown;
    }
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    const OSStatus status =
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(cfUid),
                                   &cfUid, &size, &device);
    CFRelease(cfUid);
    return status == noErr ? device : kAudioObjectUnknown;
}

/// The output channels the device really has, summed across its streams. Zero
/// when it will not say, which the caller reads as "leave it to the device".
[[nodiscard]] std::uint32_t deviceChannels(AudioObjectID device) {
    const AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration,
                                             kAudioObjectPropertyScopeOutput,
                                             kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioBufferList)) {
        return 0;
    }
    std::vector<std::byte> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr) {
        return 0;
    }
    std::uint32_t channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        channels += list->mBuffers[i].mNumberChannels;
    }
    return channels;
}

/// A layout tag for the arrangements that have one, so the spatializer is told
/// "7.1" rather than left to read a bitmap. The same WAVE-order tags Cog gives
/// its stream (OutputCoreAudio.m, -updateStreamFormat). XPCog's channel flags
/// are CoreAudio's kAudioChannelBit_* bit for bit, so anything else travels as
/// the bitmap itself.
[[nodiscard]] AudioChannelLayout layoutFor(std::uint32_t config) {
    AudioChannelLayout layout{};
    switch (config) {
        case kConfigMono: layout.mChannelLayoutTag = kAudioChannelLayoutTag_Mono; break;
        case kConfigStereo: layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo; break;
        case kConfig3Point0: layout.mChannelLayoutTag = kAudioChannelLayoutTag_WAVE_3_0; break;
        case kConfig4Point0: layout.mChannelLayoutTag = kAudioChannelLayoutTag_WAVE_4_0_A; break;
        case kConfig5Point0: layout.mChannelLayoutTag = kAudioChannelLayoutTag_WAVE_5_0_A; break;
        case kConfig5Point1: layout.mChannelLayoutTag = kAudioChannelLayoutTag_WAVE_5_1_A; break;
        case kConfig6Point1: layout.mChannelLayoutTag = kAudioChannelLayoutTag_WAVE_6_1; break;
        case kConfig7Point1: layout.mChannelLayoutTag = kAudioChannelLayoutTag_WAVE_7_1; break;
        default:
            layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
            layout.mChannelBitmap    = static_cast<AudioChannelBitmap>(config);
            break;
    }
    return layout;
}

class MacSpatialStream final : public SpatialStream {
public:
    MacSpatialStream(double sampleRate, std::uint32_t channels, SpatialSource source)
        : rate_(static_cast<std::int32_t>(std::lround(sampleRate))),
          channels_(channels),
          chunkFrames_(static_cast<std::size_t>(std::max(64.0, sampleRate * kTickSeconds))),
          source_(std::move(source)) {}

    ~MacSpatialStream() override {
        if (timer_ != nullptr) {
            dispatch_source_cancel(timer_);
        }
        // After the cancel, so no pump is left running or queued behind it: the
        // source reaches into the output's ring and tap, which are about to go.
        if (queue_ != nullptr) {
            dispatch_sync(queue_, ^{
              closed_ = true;
            });
        }
        if (flushObserver_ != nil) {
            [[NSNotificationCenter defaultCenter] removeObserver:flushObserver_];
        }
        if (synchronizer_ != nil) {
            [synchronizer_ setRate:0.0];
            [renderer_ flush];
            [synchronizer_ removeRenderer:renderer_ atTime:kCMTimeZero completionHandler:nil];
        }
        releasePending();
        if (format_ != nullptr) {
            CFRelease(format_);
        }
    }

    bool open(std::uint32_t channelConfig, const std::string& deviceUid) {
        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate       = rate_;
        asbd.mFormatID         = kAudioFormatLinearPCM;
        asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mChannelsPerFrame = channels_;
        asbd.mBitsPerChannel   = 32;
        asbd.mBytesPerFrame    = channels_ * sizeof(float);
        asbd.mFramesPerPacket  = 1;
        asbd.mBytesPerPacket   = asbd.mBytesPerFrame;

        const AudioChannelLayout layout = layoutFor(channelConfig);
        if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, sizeof(layout),
                                           &layout, 0, nullptr, nullptr,
                                           &format_) != noErr) {
            return false;
        }

        renderer_     = [[AVSampleBufferAudioRenderer alloc] init];
        synchronizer_ = [[AVSampleBufferRenderSynchronizer alloc] init];
        if (renderer_ == nil || synchronizer_ == nil) {
            return false;
        }
        // Left alone for the system default, which the renderer then follows as
        // it moves -- what an empty selection means everywhere else in the
        // player. Not assigned nil: the setter throws on it.
        if (!deviceUid.empty()) {
            NSString* uid = [NSString stringWithUTF8String:deviceUid.c_str()];
            if (uid == nil) {
                return false;
            }
            renderer_.audioOutputDeviceUniqueID = uid;
        }
        // Multichannel only. Nothing narrower comes this way, and "Spatialize
        // Stereo" is the listener's own switch in Control Center, not ours.
        renderer_.allowedAudioSpatializationFormats = AVAudioSpatializationFormatMultichannel;
        [synchronizer_ addRenderer:renderer_];

        queue_ = dispatch_queue_create("co.losno.XPCog.spatial", DISPATCH_QUEUE_SERIAL);

        // A route change -- AirPods taken out, another device made default --
        // makes the renderer drop what it holds. That audio has already left
        // the ring and cannot be had back, so the stream primes again from
        // wherever the clock had got to: a skip of up to the queue's depth, and
        // then the rest in time.
        flushObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVSampleBufferAudioRendererWasFlushedAutomaticallyNotification
                        object:renderer_
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      dispatch_async(queue_, ^{
                        if (!closed_) {
                            reprime(false);
                        }
                      });
                    }];

        lastTick_ = Clock::now();
        lastData_ = lastTick_;
        dispatch_sync(queue_, ^{
          pump();
        });

        timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
        const auto tick = static_cast<std::uint64_t>(kTickSeconds * NSEC_PER_SEC);
        dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, tick), tick,
                                  tick / 4);
        dispatch_source_set_event_handler(timer_, ^{
          pump();
        });
        dispatch_resume(timer_);
        return true;
    }

    void pause() override {
        dispatch_sync(queue_, ^{
          paused_ = true;
          if (state_ == State::Running) {
              [synchronizer_ setRate:0.0];
          }
        });
    }

    void resume() override {
        dispatch_sync(queue_, ^{
          paused_ = false;
          if (state_ == State::Running) {
              [synchronizer_ setRate:1.0];
          }
          pump();
        });
    }

    [[nodiscard]] std::uint64_t framesPlayed() const override {
        // The high-water mark as well as the clock: going back to priming sets
        // the clock to the frame the queue ran out on, which the renderer's own
        // reading may have passed by a few frames, and a clock that stepped back
        // would put a seam behind the one already announced.
        const std::int64_t now  = playedFrame();
        std::int64_t       seen = heardMax_.load(std::memory_order_relaxed);
        while (now > seen &&
               !heardMax_.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
        }
        return static_cast<std::uint64_t>(std::max(now, seen));
    }

    [[nodiscard]] double latencySeconds() const override {
        return static_cast<double>(queuedFrames_.load(std::memory_order_relaxed)) / rate_;
    }

private:
    enum class State { Priming, Running };

    struct Heard {
        std::int64_t       start;
        std::size_t        frames;
        std::vector<float> samples;
    };

    [[nodiscard]] std::int64_t playedFrame() const {
        const CMTime now = synchronizer_.currentTime;
        if (!CMTIME_IS_NUMERIC(now)) {
            return 0;
        }
        return std::max<std::int64_t>(
            0, CMTimeConvertScale(now, rate_, kCMTimeRoundingMethod_RoundTowardZero).value);
    }

    /// Back to priming, from the frame the clock has reached. `flush` for a
    /// seek, where the renderer still holds audio from the old position; an
    /// automatic flush has already emptied it. queue_ only.
    void reprime(bool flush) {
        [synchronizer_ setRate:0.0];
        if (flush) {
            [renderer_ flush];
        }
        const std::int64_t at = playedFrame();
        [synchronizer_ setRate:0.0 time:CMTimeMake(at, rate_)];
        releasePending();
        nextFrame_ = at;
        primeFrom_ = at;
        state_     = State::Priming;
        lastData_  = Clock::now();
        heard_.clear();
    }

    /// queue_ only.
    void pump() {
        if (closed_ || failed_) {
            return;
        }
        if (renderer_.status == AVQueuedSampleBufferRenderingStatusFailed) {
            failed_ = true;
            if (source_.failed) {
                source_.failed();
            }
            return;
        }

        // The gain first and always, paused or not. A fade in asked for while
        // paused has to be under way by the time the clock moves again, and it
        // is timed by the wall clock for that reason: the renderer's stands
        // still.
        const auto now = Clock::now();
        const auto elapsed =
            static_cast<std::size_t>(std::chrono::duration<double>(now - lastTick_).count() *
                                     rate_);
        lastTick_ = now;
        // Only on a change. Assigned every tick, it was a steady stream of
        // parameter changes into a renderer that was still starting up.
        if (const float gain = source_.gain(elapsed); gain != appliedGain_) {
            renderer_.volume = gain;
            appliedGain_     = gain;
        }

        if (paused_) {
            return;
        }

        publishHeard(playedFrame());

        // Whole buffers of chunkFrames_, gathered across pulls. The application's
        // ring holds about 40 ms of 7.1, so a pull often comes back with a
        // sliver -- and handed to the renderer as buffers of a few dozen
        // frames, those made every start sputter on AirPods, while Apple's own
        // player on the same file and the same headset did not. A part-filled
        // buffer waits here for the rest instead.
        while (renderer_.readyForMoreMediaData) {
            if (pendingBlock_ == nullptr && !allocatePending()) {
                break;
            }
            bool flushed = false;
            while (pendingFrames_ < chunkFrames_) {
                const std::size_t got = source_.pull(
                    pendingData_ + pendingFrames_ * channels_, chunkFrames_ - pendingFrames_,
                    flushed);
                if (flushed || got == 0) {
                    break;
                }
                pendingFrames_ += got;
                lastData_ = now;
            }
            if (flushed) {
                // A seek. The ring has dropped the old position; the renderer
                // must drop its second of it too, or the jump is heard a second
                // late -- and a part-gathered buffer of the old position goes
                // with it.
                releasePending();
                reprime(true);
                continue;
            }
            if (pendingFrames_ < chunkFrames_) {
                break;
            }
            enqueuePending();
        }

        const std::int64_t clock = playedFrame();
        // A part-gathered buffer goes short only when waiting for the rest
        // would cost more: priming that has given up on more coming, or a
        // running queue nearly out.
        if (pendingFrames_ > 0) {
            const bool stalled =
                std::chrono::duration<double>(now - lastData_).count() > kPrimeGiveUpSeconds;
            const bool low = state_ == State::Running &&
                             nextFrame_ - clock < static_cast<std::int64_t>(chunkFrames_ * 2);
            if (stalled || low) {
                enqueuePending();
            }
        }
        if (state_ == State::Priming) {
            const bool queued = nextFrame_ > primeFrom_;
            const bool enough = renderer_.hasSufficientMediaDataForReliablePlaybackStart ||
                                !renderer_.readyForMoreMediaData;
            const bool noMoreSoon = std::chrono::duration<double>(now - lastData_).count() >
                                    kPrimeGiveUpSeconds;
            if (queued && (enough || noMoreSoon)) {
                [synchronizer_ setRate:1.0 time:CMTimeMake(primeFrom_, rate_)];
                state_ = State::Running;
            }
        } else if (clock >= nextFrame_) {
            // Ran dry: the ring had nothing when the renderer needed it. Stop
            // the clock on the last frame queued rather than let it run on over
            // silence, and prime again -- otherwise the next buffer would be
            // stamped behind the clock and dropped.
            [synchronizer_ setRate:0.0 time:CMTimeMake(nextFrame_, rate_)];
            primeFrom_ = nextFrame_;
            state_     = State::Priming;
        }

        queuedFrames_.store(std::max<std::int64_t>(0, nextFrame_ - clock),
                            std::memory_order_relaxed);
    }

    /// queue_ only, like everything that touches the pending buffer.
    bool allocatePending() {
        const std::size_t bytes = chunkFrames_ * channels_ * sizeof(float);
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes,
                                               kCFAllocatorDefault, nullptr, 0, bytes,
                                               kCMBlockBufferAssureMemoryNowFlag,
                                               &pendingBlock_) != kCMBlockBufferNoErr) {
            pendingBlock_ = nullptr;
            return false;
        }
        char* data = nullptr;
        if (CMBlockBufferGetDataPointer(pendingBlock_, 0, nullptr, nullptr, &data) !=
            kCMBlockBufferNoErr) {
            releasePending();
            return false;
        }
        pendingData_   = reinterpret_cast<float*>(data);
        pendingFrames_ = 0;
        return true;
    }

    void releasePending() {
        if (pendingBlock_ != nullptr) {
            CFRelease(pendingBlock_);
        }
        pendingBlock_  = nullptr;
        pendingData_   = nullptr;
        pendingFrames_ = 0;
    }

    /// The pending buffer to the renderer, at however many frames it holds.
    void enqueuePending() {
        const std::size_t frames = pendingFrames_;
        CMSampleBufferRef sample = nullptr;
        const OSStatus    status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            kCFAllocatorDefault, pendingBlock_, format_, static_cast<CMItemCount>(frames),
            CMTimeMake(nextFrame_, rate_), nullptr, &sample);
        if (status == noErr) {
            [renderer_ enqueueSampleBuffer:sample];
            CFRelease(sample);
            // Copied, not held: the renderer owns the block now, and the tap
            // wants these samples only once the clock reaches them.
            heard_.push_back(Heard{nextFrame_, frames,
                                   std::vector<float>(pendingData_,
                                                      pendingData_ + frames * channels_)});
            nextFrame_ += static_cast<std::int64_t>(frames);
        }
        releasePending();
    }

    /// Hands the tap every buffer the clock has reached. queue_ only.
    void publishHeard(std::int64_t played) {
        while (!heard_.empty() && heard_.front().start <= played) {
            const Heard& front = heard_.front();
            if (source_.heard) {
                source_.heard(front.samples.data(), front.frames);
            }
            heard_.pop_front();
        }
    }

    const std::int32_t  rate_;
    const std::uint32_t channels_;
    const std::size_t   chunkFrames_;
    SpatialSource       source_;

    CMAudioFormatDescriptionRef       format_        = nullptr;
    AVSampleBufferAudioRenderer*      renderer_      = nil;
    AVSampleBufferRenderSynchronizer* synchronizer_  = nil;
    dispatch_queue_t                  queue_         = nullptr;
    dispatch_source_t                 timer_         = nullptr;
    id                                flushObserver_ = nil;

    // queue_ only, or under a dispatch_sync onto it.
    State             state_     = State::Priming;
    bool              paused_    = false;
    bool              closed_    = false;
    bool              failed_    = false;
    std::int64_t      nextFrame_ = 0;
    std::int64_t      primeFrom_ = 0;
    Clock::time_point lastTick_{};
    Clock::time_point lastData_{};
    std::deque<Heard> heard_;
    float             appliedGain_ = 1.0F;
    CMBlockBufferRef  pendingBlock_  = nullptr;
    float*            pendingData_   = nullptr;
    std::size_t       pendingFrames_ = 0;

    mutable std::atomic<std::int64_t> heardMax_{0};
    std::atomic<std::int64_t>         queuedFrames_{0};
};

}  // namespace

bool spatialStreamWanted(std::uint32_t channels, const std::string& deviceUid) {
    if (channels <= 2) {
        return false;
    }
    const AudioObjectID device = resolveDevice(deviceUid);
    if (device == kAudioObjectUnknown) {
        return false;
    }
    const std::uint32_t available = deviceChannels(device);
    return available > 0 && available < channels;
}

std::unique_ptr<SpatialStream> openSpatialStream(double sampleRate, std::uint32_t channels,
                                                 std::uint32_t      channelConfig,
                                                 const std::string& deviceUid,
                                                 SpatialSource      source) {
    auto stream = std::make_unique<MacSpatialStream>(sampleRate, channels, std::move(source));
    if (!stream->open(channelConfig, deviceUid)) {
        return nullptr;
    }
    return stream;
}

}  // namespace detail
}  // namespace xpcog
