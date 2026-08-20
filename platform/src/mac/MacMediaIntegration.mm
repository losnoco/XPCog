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

#include <utility>

namespace xpcog::platform {
namespace {

[[nodiscard]] NSString* toNS(const std::string& text) {
    // initWithBytes rather than stringWithUTF8String: the latter takes a C string
    // and would stop at an embedded NUL. Strings crossing this boundary are UTF-8
    // by contract; nil back means they were not, and an empty string is a better
    // answer than a nil in a dictionary literal.
    NSString* value = [[NSString alloc] initWithBytes:text.data()
                                               length:static_cast<NSUInteger>(text.size())
                                             encoding:NSUTF8StringEncoding];
    return value != nil ? value : @"";
}

/// The artwork as an NSImage.
///
/// These are the image's own encoded bytes, straight out of the file the music
/// came in, so NSImage decodes them itself. This used to take a decoded image and
/// re-encode it as PNG purely to get back to bytes, which is a round trip that
/// existed only because the type crossing the boundary was the wrong one.
[[nodiscard]] NSImage* toNSImage(const std::vector<std::byte>& artwork) {
    if (artwork.empty()) {
        return nil;
    }
    NSData* data = [NSData dataWithBytes:artwork.data()
                                  length:static_cast<NSUInteger>(artwork.size())];
    return [[NSImage alloc] initWithData:data];
}

class MacMediaIntegration final : public MediaIntegration {
public:
    explicit MacMediaIntegration(Dispatcher dispatch)
        : MediaIntegration(std::move(dispatch)) {
        MPRemoteCommandCenter* centre = [MPRemoteCommandCenter sharedCommandCenter];

        // The blocks run on the main thread, so this is already the user
        // interface's and the hop publishOnUiThread() makes is a deferral to the
        // next turn of the loop rather than a thread change. Going through it
        // anyway keeps one rule for all three platforms -- an implementation
        // never publishes directly -- and means nothing here has to be revisited
        // if that guarantee ever softens.
        //
        // `this` outlives the blocks because the destructor removes every target.
        [centre.playCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            publishOnUiThread(playRequested);
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [centre.pauseCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            publishOnUiThread(pauseRequested);
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        // The physical play/pause key sends this one, not play or pause.
        [centre.togglePlayPauseCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            publishOnUiThread(playPauseRequested);
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [centre.stopCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            publishOnUiThread(stopRequested);
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [centre.nextTrackCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            publishOnUiThread(nextRequested);
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [centre.previousTrackCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
            publishOnUiThread(previousRequested);
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        // Dragging the scrubber in the Now Playing widget.
        centre.changePlaybackPositionCommand.enabled = YES;
        [centre.changePlaybackPositionCommand
            addTargetWithHandler:^(MPRemoteCommandEvent* event) {
                auto* positionEvent =
                    static_cast<MPChangePlaybackPositionCommandEvent*>(event);
                publishOnUiThread(seekRequested,
                                  static_cast<double>(positionEvent.positionTime));
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

std::unique_ptr<MediaIntegration> MediaIntegration::create(Dispatcher dispatch,
                                                           void* nativeWindow) {
    // Unused here: MPRemoteCommandCenter is per-process and binds to no window,
    // unlike SMTC. The parameter is in the signature because Windows cannot do
    // without it.
    (void)nativeWindow;
    return std::make_unique<MacMediaIntegration>(std::move(dispatch));
}

}  // namespace xpcog::platform
