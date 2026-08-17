// Cue sheet parsing. Port of Cog Plugins/CueSheet/CueSheet.m.

#pragma once

#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/TrackProperties.hpp"
#include "xpcog/core/Url.hpp"

#include <string>
#include <vector>

namespace xpcog::codecs {

struct CueTrack {
    /// Track number exactly as written ("01", "02"), because that string is the
    /// URL fragment used to address the track.
    std::string track;
    /// The audio file this track lives in.
    Url url;

    /// INDEX 01 position. Usually seconds; a bare number is a sample count
    /// instead, which is what `timeInSamples` distinguishes.
    double time          = 0.0;
    bool   timeInSamples = false;

    std::string artist;
    std::string album;
    std::string title;
    std::string genre;
    std::string year;

    ReplayGainInfo replayGain;

    /// Start frame within the audio file, given its sample rate.
    [[nodiscard]] std::int64_t startFrame(double sampleRate) const {
        return timeInSamples ? static_cast<std::int64_t>(time)
                             : static_cast<std::int64_t>(time * sampleRate);
    }

    /// Tags for this track, as a decoder would report them.
    [[nodiscard]] MetadataMap metadata() const;
};

class CueSheet {
public:
    /// `sheetUrl` locates the sheet itself; relative FILE paths resolve against
    /// its directory. For an embedded sheet, pass the audio file's own URL.
    [[nodiscard]] static CueSheet parse(const std::string& text, const Url& sheetUrl);

    [[nodiscard]] const std::vector<CueTrack>& tracks() const noexcept { return tracks_; }
    [[nodiscard]] bool empty() const noexcept { return tracks_.empty(); }

    /// The track addressed by a URL fragment, or nullptr.
    [[nodiscard]] const CueTrack* findTrack(std::string_view fragment) const;

    /// The track after `index`, when it lives in the same audio file -- that is
    /// what bounds the previous track's end. nullptr when `index` is last, or
    /// when the next track is in a different file.
    [[nodiscard]] const CueTrack* nextInSameFile(std::size_t index) const;

    [[nodiscard]] std::size_t indexOf(const CueTrack* track) const;

private:
    std::vector<CueTrack> tracks_;
};

}  // namespace xpcog::codecs
