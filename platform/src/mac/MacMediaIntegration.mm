// macOS media keys and Now Playing, via MediaPlayer.framework.
//
// Cog does this with a 900-line vendored CogRemoteControl (an SPMediaKeyTap
// fork) that installs a CGEventTap on the HID stream, plus its own
// MPNowPlayingInfoCenter code. The event tap needed Accessibility permission,
// fought other players for the keys, and has been unnecessary since macOS
// 10.12.2: registering handlers on MPRemoteCommandCenter is what makes the keys
// arrive, and it arbitrates between applications properly.

#include "xpcog/platform/MediaIntegration.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>

#include <QBuffer>
#include <QByteArray>

namespace xpcog::platform {
namespace {

[[nodiscard]] NSString* toNS(const QString& text) {
    return text.toNSString();
}

/// A QImage as an NSImage, via PNG. Going through a byte buffer rather than
/// poking at QImage's bits avoids having to match Qt's format to a
/// CGBitmapInfo, which is a per-format table nobody wants to maintain for a
/// picture that gets scaled to 64 points anyway.
[[nodiscard]] NSImage* toNSImage(const QImage& image) {
    if (image.isNull()) {
        return nil;
    }
    QByteArray encoded;
    QBuffer    buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        return nil;
    }
    NSData* data = [NSData dataWithBytes:encoded.constData()
                                  length:static_cast<NSUInteger>(encoded.size())];
    return [[NSImage alloc] initWithData:data];
}

class MacMediaIntegration final : public MediaIntegration {
public:
    explicit MacMediaIntegration(QObject* parent) : MediaIntegration(parent) {
        MPRemoteCommandCenter* centre = [MPRemoteCommandCenter sharedCommandCenter];

        // Blocks run on the main thread, which is where this object lives, so
        // emitting straight from them is safe. `this` outlives them because the
        // destructor removes every target.
        [centre.playCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            emit playRequested();
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [centre.pauseCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            emit pauseRequested();
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        // The physical play/pause key sends this one, not play or pause.
        [centre.togglePlayPauseCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            emit playPauseRequested();
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [centre.stopCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            emit stopRequested();
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [centre.nextTrackCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            emit nextRequested();
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [centre.previousTrackCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            emit previousRequested();
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        // Dragging the scrubber in the Now Playing widget.
        centre.changePlaybackPositionCommand.enabled = YES;
        [centre.changePlaybackPositionCommand
            addTargetWithHandler:^(MPRemoteCommandEvent* event) {
                auto* positionEvent =
                    static_cast<MPChangePlaybackPositionCommandEvent*>(event);
                emit seekRequested(positionEvent.positionTime);
                return MPRemoteCommandHandlerStatusSuccess;
            }];

        // Explicitly off. Left enabled they appear as buttons that do nothing,
        // and seekForward in particular is what the OS falls back to when
        // nextTrack is disabled.
        centre.skipForwardCommand.enabled  = NO;
        centre.skipBackwardCommand.enabled = NO;
        centre.seekForwardCommand.enabled  = NO;
        centre.seekBackwardCommand.enabled = NO;
        centre.changeRepeatModeCommand.enabled  = NO;
        centre.changeShuffleModeCommand.enabled = NO;
        centre.likeCommand.enabled              = NO;
        centre.dislikeCommand.enabled           = NO;
        centre.ratingCommand.enabled            = NO;
    }

    ~MacMediaIntegration() override {
        MPRemoteCommandCenter* centre = [MPRemoteCommandCenter sharedCommandCenter];
        for (MPRemoteCommand* command in @[
                 centre.playCommand, centre.pauseCommand, centre.togglePlayPauseCommand,
                 centre.stopCommand, centre.nextTrackCommand, centre.previousTrackCommand,
                 centre.changePlaybackPositionCommand
             ]) {
            [command removeTarget:nil];
        }
        clear();
    }

    void setNowPlaying(const NowPlayingInfo& info) override {
        NSMutableDictionary* entry = [NSMutableDictionary dictionary];
        entry[MPMediaItemPropertyTitle]      = toNS(info.title);
        entry[MPMediaItemPropertyArtist]     = toNS(info.artist);
        entry[MPMediaItemPropertyAlbumTitle] = toNS(info.album);
        entry[MPMediaItemPropertyPlaybackDuration] = @(info.duration);
        entry[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(info.position);
        entry[MPNowPlayingInfoPropertyPlaybackRate]        = @(1.0);
        entry[MPNowPlayingInfoPropertyMediaType] =
            @(MPNowPlayingInfoMediaTypeAudio);

        if (NSImage* art = toNSImage(info.artwork); art != nil) {
            entry[MPMediaItemPropertyArtwork] = [[MPMediaItemArtwork alloc]
                initWithBoundsSize:art.size
                    requestHandler:^NSImage*(CGSize) { return art; }];
        }

        // Kept, because setPlaybackState() has to rewrite the whole dictionary:
        // assigning nowPlayingInfo replaces it rather than merging.
        entry_ = entry;
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = entry;
    }

    void setPlaybackState(bool playing, bool paused, double position) override {
        MPNowPlayingInfoCenter* centre = [MPNowPlayingInfoCenter defaultCenter];

        if (!playing) {
            centre.playbackState = MPNowPlayingPlaybackStateStopped;
            return;
        }
        centre.playbackState =
            paused ? MPNowPlayingPlaybackStatePaused : MPNowPlayingPlaybackStatePlaying;

        if (entry_ == nil) {
            return;
        }
        entry_[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position);
        // The rate is what lets the widget count on its own between updates. A
        // paused track must report zero, or the display keeps ticking forward
        // from a position that is not moving.
        entry_[MPNowPlayingInfoPropertyPlaybackRate] = @(paused ? 0.0 : 1.0);
        centre.nowPlayingInfo = entry_;
    }

    void clear() override {
        entry_ = nil;
        MPNowPlayingInfoCenter* centre = [MPNowPlayingInfoCenter defaultCenter];
        centre.nowPlayingInfo = nil;
        centre.playbackState  = MPNowPlayingPlaybackStateStopped;
    }

private:
    NSMutableDictionary* entry_ = nil;
};

}  // namespace

MediaIntegration* MediaIntegration::create(QObject* parent) {
    return new MacMediaIntegration(parent);
}

}  // namespace xpcog::platform
