#include "LyricsText.hpp"

#include "Translations.hpp"

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace xpcog::app {
namespace {

/// Shown instead of an empty box when the file carries no lyrics.
///
/// A divergence from Cog, which leaves its window blank. A blank pane is
/// ambiguous in a way a blank *window* is not: the window was opened
/// deliberately and can only be about the one thing, while a pane sits in the
/// layout all the time and an empty one reads as "still loading" or "broken"
/// rather than as an answer. Saying it costs one line and removes the question.
constexpr const char* kNoLyrics = XPCOG_TRANSLATE("This file carries no lyrics.");
constexpr const char* kNoTrack  = XPCOG_TRANSLATE("Nothing selected or playing.");

// The online states. Each starts by restating that the file has none, because
// that is still true and still the first thing a reader wants to know: what
// follows is what was done about it.
constexpr const char* kLooking = XPCOG_TRANSLATE("This file carries no lyrics. Asking LRCLIB...");
constexpr const char* kNotFound =
    XPCOG_TRANSLATE("This file carries no lyrics, and LRCLIB has none for it.");
constexpr const char* kInstrumental =
    XPCOG_TRANSLATE("This file carries no lyrics. LRCLIB lists it as an instrumental.");
constexpr const char* kUnreachable =
    XPCOG_TRANSLATE("This file carries no lyrics. LRCLIB could not be reached.");
constexpr const char* kBusy =
    XPCOG_TRANSLATE("This file carries no lyrics. LRCLIB is busy; try again in a moment.");
constexpr const char* kRefused =
    XPCOG_TRANSLATE("This file carries no lyrics. LRCLIB could not answer (HTTP %d).");

constexpr const char* kFromLrclib  = XPCOG_TRANSLATE("From LRCLIB");
constexpr const char* kFromSidecar = XPCOG_TRANSLATE("From the .lrc file beside the track");

/// WCAG relative luminance of an sRGB colour.
[[nodiscard]] double luminance(Rgb colour) {
    const auto channel = [](std::uint8_t value) {
        const double c = value / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.red) + 0.7152 * channel(colour.green) +
           0.0722 * channel(colour.blue);
}

[[nodiscard]] double contrast(Rgb a, Rgb b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

[[nodiscard]] LyricsText plain(const std::string& words, std::string source) {
    LyricsText text;
    text.body   = normaliseLyrics(words);
    text.source = std::move(source);
    return text;
}

[[nodiscard]] LyricsText timed(SyncedLyrics lyrics, std::string source) {
    LyricsText text;
    text.body   = syncedDisplayText(lyrics);
    text.source = std::move(source);
    text.synced = std::move(lyrics);
    return text;
}

/// Timed words as the pane should draw them: followed, or stripped to text.
[[nodiscard]] LyricsText either(SyncedLyrics lyrics, std::string source, bool asTimed) {
    LyricsText text = asTimed ? timed(std::move(lyrics), std::move(source))
                              : plain(lyrics.plainText(), std::move(source));
    text.timeable   = true;
    return text;
}

}  // namespace

std::string normaliseLyrics(std::string text) {
    // CRLF and lone CR both become LF. Windows taggers write CRLF, some write a
    // bare CR, and a `\r` that reaches a GTK text control is drawn as a box
    // rather than as a line break -- so this is a correctness fix on Linux and a
    // tidying one elsewhere.
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue;  // the LF of a CRLF pair carries the break
            }
            out.push_back('\n');
            continue;
        }
        out.push_back(text[i]);
    }

    // Trailing blank lines are common -- a tagger padding the field, or a lyrics
    // site's copy-paste -- and in a scrolling control they are indistinguishable
    // from the song having more to say.
    const auto end = out.find_last_not_of(" \t\n");
    if (end == std::string::npos) {
        return {};
    }
    out.erase(end + 1);

    // Leading blank lines push the first line out of view for no reason.
    const auto begin = out.find_first_not_of(" \t\n");
    if (begin != std::string::npos && begin > 0) {
        out.erase(0, begin);
    }
    return out;
}

std::string syncedDisplayText(const SyncedLyrics& lyrics) {
    // Every line, breaks included, and nothing trimmed from the ends: line N
    // of this text is line N of the file, which is the whole of what the
    // highlight needs to find it.
    std::string out;
    for (std::size_t i = 0; i < lyrics.lines.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += lyrics.lines[i].text;
    }
    return out;
}

Rgb readableOn(Rgb colour, Rgb background, Rgb text) {
    constexpr double kReadable = 4.5;
    if (contrast(colour, background) >= kReadable) {
        return colour;
    }
    // Tenths are fine enough: the eye cannot tell adjacent steps apart, and the
    // loop ends at `text` itself, which is readable by construction.
    for (int step = 1; step <= 10; ++step) {
        const double t   = step / 10.0;
        const auto   mix = [t](std::uint8_t from, std::uint8_t to) {
            return static_cast<std::uint8_t>(std::lround(from + (to - from) * t));
        };
        const Rgb mixed{mix(colour.red, text.red), mix(colour.green, text.green),
                        mix(colour.blue, text.blue)};
        if (contrast(mixed, background) >= kReadable) {
            return mixed;
        }
    }
    return text;
}

LyricsPresenter::LyricsPresenter() : alive_(std::make_shared<int>(0)) {}

LyricsPresenter::~LyricsPresenter() = default;

void LyricsPresenter::setLookup(LyricsLookup* lookup) {
    lookup_ = lookup;
    // Whatever was being waited for is being waited for from a lookup that
    // may no longer be the one asked; the redraw the caller does next asks
    // again, and the old answer, if it comes, finds no key to match.
    awaiting_.clear();
}

std::optional<LyricsQuery> LyricsPresenter::queryFor(const PlaylistEntry& entry) {
    // No artist means nothing to ask: the service matches on both, and a title
    // alone -- "Track 3", or a stream's ICY name -- would match anything or
    // nothing. The service's own rule, applied here so the request is not
    // made only to be refused. rawTitle rather than title(), which falls back
    // to the file name: "01 - Something.flac" is not a question worth a
    // round trip and a week in the cache as "not found".
    if (entry.artist.empty() || entry.rawTitle.empty()) {
        return std::nullopt;
    }
    LyricsQuery query;
    query.title  = entry.rawTitle;
    query.artist = entry.artist.str();
    query.album  = entry.album.str();
    // Unknown lengths -- live streams -- go as zero, which the client leaves
    // out of the request rather than sending as a number that cannot match.
    query.duration = entry.duration();
    return query;
}

std::string LyricsPresenter::keyOf(const LyricsText& text) {
    return text.heading + (text.synced ? "\n\x01" : "\n") + text.body;
}

LyricsText LyricsPresenter::fromFile(const PlaylistEntry& entry) const {
    const std::string& tag = entry.unsyncedLyrics.str();
    if (auto lyrics = parseLrc(tag)) {
        return either(std::move(*lyrics), std::string{}, timed_);
    }
    // Shown as text, the tag's words are as good as the file's and they are
    // the listener's own; only timing put the file ahead of them.
    if (!timed_ && !normaliseLyrics(tag).empty()) {
        return plain(tag, std::string{});
    }
    if (const auto sidecar = readSidecarLrc(entry.url)) {
        if (auto lyrics = parseLrc(*sidecar)) {
            return either(std::move(*lyrics), tr(kFromSidecar), timed_);
        }
    }
    return plain(tag, std::string{});
}

LyricsText LyricsPresenter::bodyOf(std::string heading, const LyricsLookup::Answer& answer) const {
    LyricsText text;
    switch (answer.outcome) {
        case LyricsLookup::Outcome::Found: {
            auto lyrics = parseLrc(answer.synced);
            if (lyrics && timed_) {
                text = timed(std::move(*lyrics), tr(kFromLrclib));
            } else if (normaliseLyrics(answer.lyrics).empty() && lyrics) {
                // The service derives a plain copy from a timed one, but an
                // entry uploaded with only the timed file is not guaranteed
                // to carry it.
                text = plain(lyrics->plainText(), tr(kFromLrclib));
            } else {
                text = plain(answer.lyrics, tr(kFromLrclib));
            }
            text.timeable = lyrics.has_value() || text.synced.has_value();
            text.heading  = std::move(heading);
            return text;
        }
        case LyricsLookup::Outcome::Instrumental:
            text.body = tr(kInstrumental);
            break;
        case LyricsLookup::Outcome::NotFound:
            text.body = tr(kNotFound);
            break;
        case LyricsLookup::Outcome::Failed:
            switch (answer.error.kind) {
                case LyricsError::Kind::Transport:
                    text.body = tr(kUnreachable);
                    break;
                case LyricsError::Kind::Transient:
                    text.body = tr(kBusy);
                    break;
                default:
                    text.body = trf(kRefused, answer.error.code);
                    break;
            }
            break;
    }
    text.heading = std::move(heading);
    return text;
}

std::optional<LyricsText> LyricsPresenter::show(const PlaylistEntry*                    entry,
                                                std::function<void(const LyricsText&)> answered) {
    LyricsText text;

    // Set when the track on screen has to be asked about; the key the answer
    // must still match to be drawn.
    std::optional<LyricsQuery> ask;
    std::string                askKey;

    if (entry == nullptr) {
        text.body = tr(kNoTrack);
    } else {
        std::string heading = entry->artist.empty()
                                  ? entry->title()
                                  : entry->artist.str() + " \xE2\x80\x94 " + entry->title();
        text         = fromFile(*entry);
        text.heading = std::move(heading);
        if (text.body.empty()) {
            text.body = tr(kNoLyrics);
            // The file has none. The service is asked only now -- a tag is the
            // listener's own and is never second-guessed -- and only about a
            // track it could know.
            if (lookup_ != nullptr) {
                if (auto query = queryFor(*entry)) {
                    askKey = LyricsLookup::keyOf(*query);
                    if (const auto known = lookup_->cached(*query)) {
                        text = bodyOf(text.heading, *known);
                    } else {
                        text.body = tr(kLooking);
                        ask       = std::move(query);
                    }
                }
            }
        }
    }

    // Keyed on both halves: the same lyrics under a different heading is a
    // different track -- two rips of one song, or a file appearing twice. A
    // redraw of a track still being asked about lands here too, with the same
    // key, and must not ask again. So does the track on screen starting or
    // stopping, which changes whether it is followed and nothing else.
    const std::string key = keyOf(text);
    if (key == shownKey_) {
        return std::nullopt;
    }
    shownKey_ = key;

    // Answered, or being answered, for a different track than before: the key
    // an arriving answer has to match is this one now, or none.
    awaiting_ = ask ? askKey : std::string{};
    if (ask) {
        // The lookup deduplicates a query already in flight and remembers
        // every answer, so this is cheap to call for a track just scrolled
        // past and back to. The handler is dispatched on the interface thread;
        // the weak pointer is for the one dispatched after the pane has gone.
        std::weak_ptr<int> guard   = alive_;
        std::string        heading = text.heading;
        lookup_->lookup(*ask, [this, guard, askKey, heading, answered = std::move(answered)](
                                  const LyricsLookup::Answer& answer) {
            if (guard.expired()) {
                return;
            }
            // An answer for a track the listener has moved off. It is in the
            // lookup's cache for when they come back; drawing it now would put
            // one song's words under another's name.
            if (askKey != awaiting_) {
                return;
            }
            awaiting_.clear();
            LyricsText updated = bodyOf(heading, answer);
            shownKey_          = keyOf(updated);
            if (answered) {
                answered(updated);
            }
        });
    }
    return text;
}

}  // namespace xpcog::app
