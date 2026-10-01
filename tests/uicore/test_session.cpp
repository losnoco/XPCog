// The composition root, driven without a window.
//
// Session is what both frontends stand on, and these cases are what let it be
// changed for one frontend's sake without the other finding out at run time:
// the scan queue and what it says, a track played to the end through an
// offline output, the signals a window would redraw from, the effects a
// setting has below the window, and what a session leaves behind for the next.
//
// The dispatcher here is a queue this thread drains, which is what a toolkit's
// event loop is from the session's point of view. Nothing is asserted from a
// worker thread; everything the session publishes arrives through it.

#include "Session.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/library/Library.hpp"

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <random>
#include <thread>
#include <vector>

using namespace xpcog;
using xpcog::app::Session;

namespace {

constexpr double kSampleRate = 44100.0;

/// A directory of this process's own, and gone when it exits. ctest runs
/// each case as a process of its own and several at once, so a directory
/// named for the suite alone is wiped by one case while another is writing
/// its fixture into it.
struct FixtureDir {
    FixtureDir()
        : path(std::filesystem::temp_directory_path() /
               ("xpcog-session-tests-" + std::to_string(std::random_device{}()))) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~FixtureDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

std::filesystem::path fixtureDir() {
    static const FixtureDir dir;
    return dir.path;
}

/// A short stereo sine as a 16-bit WAV, then FLAC through the encoder on PATH.
/// nullopt when there is no encoder, so the suite skips rather than fails.
std::optional<std::filesystem::path> makeFlac(const std::string& name, double seconds,
                                              double freq) {
    const auto wav  = fixtureDir() / (name + ".wav");
    const auto flac = fixtureDir() / (name + ".flac");

    const int                 frames = static_cast<int>(seconds * kSampleRate);
    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const auto   v = static_cast<std::int16_t>(16000.0 * std::sin(test::kTwoPi * freq * t));
        samples.push_back(v);
        samples.push_back(v);
    }
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * 2);

    std::FILE* f = std::fopen(wav.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<std::uint32_t>(kSampleRate));
    u32(static_cast<std::uint32_t>(kSampleRate) * 4);
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), 1, dataBytes, f);
    std::fclose(f);

    const std::string command = "flac -s -f --totally-silent -T TITLE=" + name +
                                " -T \"ARTIST=Session Test\" -T ALBUM=Fixtures -o \"" +
                                flac.string() + "\" \"" + wav.string() + "\"" +
                                test::kSilenceStderr;
    if (std::system(command.c_str()) != 0 || !std::filesystem::exists(flac)) {
        return std::nullopt;
    }
    return flac;
}

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

/// The interface thread, as the session sees it: a queue drained here.
class Loop {
public:
    Dispatcher dispatcher() {
        return [this](std::function<void()> task) {
            const std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        };
    }

    /// Runs what has been queued. Returns whether anything ran.
    bool drain() {
        bool any = false;
        for (;;) {
            std::function<void()> task;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    return any;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
            any = true;
        }
    }

    /// Drains and ticks until `done` answers true or the deadline passes.
    template <typename Predicate>
    bool waitFor(Session& session, Predicate done, std::chrono::milliseconds deadline) {
        const auto until = std::chrono::steady_clock::now() + deadline;
        for (;;) {
            drain();
            session.tick();
            if (done()) {
                return true;
            }
            if (std::chrono::steady_clock::now() > until) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

private:
    std::mutex                        mutex_;
    std::deque<std::function<void()>> queue_;
};

/// A session over a fresh data directory, an offline output paced at eight
/// times real time, and a memory settings store.
struct Harness {
    Harness()
        : store(makeMemorySettingsStore()),
          settings(*store),
          data(fixtureDir() / ("data-" + std::to_string(counter++))),
          session(registry(), settings, loop.dispatcher(), options()) {
        subscriptions.push_back(session.status.connect(
            [this](const std::string& text) { statusLines.push_back(text); }));
        subscriptions.push_back(session.scanStarted.connect([this] { ++scansStarted; }));
        subscriptions.push_back(session.scanFinished.connect([this] { ++scansFinished; }));
        subscriptions.push_back(session.trackChanged.connect(
            [this](TrackId id, const PlaylistEntry* entry, bool looping) {
                tracks.push_back({id, entry != nullptr ? entry->title() : "", looping});
            }));
        subscriptions.push_back(session.playbackStateChanged.connect(
            [this](bool playing, bool paused) { states.push_back({playing, paused}); }));
        subscriptions.push_back(session.positionChanged.connect(
            [this](double seconds, double) { lastPosition = seconds; }));
        subscriptions.push_back(session.announceTrack.connect(
            [this](const std::string&, const std::string& body,
                   const std::shared_ptr<const std::vector<std::byte>>&) {
                announcements.push_back(body);
            }));
        subscriptions.push_back(session.revealRequested.connect(
            [this](TrackId id) { revealed.push_back(id); }));
        subscriptions.push_back(session.effectApplied.connect(
            [this](app::Effect effect, const std::string&) { effects.push_back(effect); }));
        subscriptions.push_back(session.volumeChanged.connect(
            [this](double gain) { volumes.push_back(gain); }));
    }

    Session::Options options() const {
        Session::Options o;
        o.dataDirectory  = data.string();
        o.cacheDirectory = (data / "cache").string();
        o.makeOutput     = [](RingBuffer& ring) { return makeOfflineOutput(ring, 8.0); };
        std::filesystem::create_directories(data);
        return o;
    }

    /// Adds `paths` and waits for the scan to land in the playlist.
    void add(const std::vector<std::filesystem::path>& paths) {
        std::vector<Url> urls;
        for (const auto& path : paths) {
            urls.push_back(Url::fromLocalPath(path));
        }
        const int before = scansFinished;
        session.addUrls(urls);
        REQUIRE(loop.waitFor(
            session, [&] { return scansFinished > before && !session.scanning(); },
            std::chrono::seconds(20)));
    }

    struct TrackEvent {
        TrackId     id;
        std::string title;
        bool        looping;
    };
    struct StateEvent {
        bool playing;
        bool paused;
    };

    static inline int counter = 0;

    Loop                            loop;
    std::unique_ptr<ISettingsStore> store;
    Settings                        settings;
    std::filesystem::path           data;
    Session                         session;

    std::vector<std::string> statusLines;
    int                      scansStarted  = 0;
    int                      scansFinished = 0;
    std::vector<TrackEvent>  tracks;
    std::vector<StateEvent>  states;
    double                   lastPosition = -1.0;
    std::vector<std::string> announcements;
    std::vector<TrackId>     revealed;
    std::vector<app::Effect> effects;
    std::vector<double>      volumes;

    std::vector<Subscription> subscriptions;
};

}  // namespace

TEST_CASE("a folder is scanned into the playlist and the status says so", "[uicore][session]") {
    if (!test::haveTool("flac")) {
        SKIP("flac encoder not on PATH");
    }
    const auto one = makeFlac("one", 1.0, 440.0);
    const auto two = makeFlac("two", 1.0, 660.0);
    REQUIRE(one);
    REQUIRE(two);

    Harness h;
    h.add({*one, *two});

    CHECK(h.session.playlist().size() == 2);
    CHECK(h.scansStarted == 1);
    CHECK(h.scansFinished == 1);
    REQUIRE(!h.statusLines.empty());
    // The summary, in the shape the status bar shows: two tracks and a clock.
    CHECK(h.statusLines.back() == h.session.statusSummary());
    CHECK(h.statusLines.back().find("2 tracks") == 0);

    // The library has a first-seen row for each, created by the scan and not
    // by the first play.
    REQUIRE(h.session.library() != nullptr);
    const PlaylistEntry& first = h.session.playlist().entries().front();
    CHECK(h.session.library()->playCount(first.artist, first.album, first.title()).has_value());
}

TEST_CASE("two scans queue rather than interleave", "[uicore][session]") {
    if (!test::haveTool("flac")) {
        SKIP("flac encoder not on PATH");
    }
    const auto one = makeFlac("queued-one", 0.5, 440.0);
    const auto two = makeFlac("queued-two", 0.5, 660.0);
    REQUIRE(one);
    REQUIRE(two);

    Harness h;
    h.session.addUrls({Url::fromLocalPath(*one)});
    h.session.addUrls({Url::fromLocalPath(*two)});
    // Only one scan is running; the second waits.
    CHECK(h.scansStarted == 1);
    REQUIRE(h.loop.waitFor(
        h.session, [&] { return h.scansFinished == 2 && !h.session.scanning(); },
        std::chrono::seconds(20)));
    CHECK(h.scansStarted == 2);
    REQUIRE(h.session.playlist().size() == 2);
    // In the order they were asked for.
    CHECK(h.session.playlist().entries()[0].title() == "queued-one");
    CHECK(h.session.playlist().entries()[1].title() == "queued-two");
}

TEST_CASE("a track plays through and the window is told each step", "[uicore][session]") {
    if (!test::haveTool("flac")) {
        SKIP("flac encoder not on PATH");
    }
    // Long enough that, at eight times real time, several 250 ms ticks land
    // inside it.
    const auto one = makeFlac("plays", 8.0, 440.0);
    REQUIRE(one);

    Harness h;
    h.settings.setNotificationsEnable(true);
    // Cog's default is repeat-all, under which a playlist of one never ends.
    h.settings.setRepeatMode(static_cast<int>(RepeatMode::None));
    h.session.settingChanged("repeat");
    h.add({*one});
    const TrackId id = h.session.playlist().entries().front().id;

    h.session.playback().playTrack(id);
    REQUIRE(h.loop.waitFor(
        h.session, [&] { return !h.tracks.empty() && h.tracks.front().id == id; },
        std::chrono::seconds(10)));
    CHECK(h.session.currentTrack() == id);
    CHECK(h.tracks.front().title == "plays");
    CHECK_FALSE(h.tracks.front().looping);

    // Announced once, whatever the seam does.
    REQUIRE(h.loop.waitFor(h.session, [&] { return !h.announcements.empty(); },
                           std::chrono::seconds(5)));
    CHECK(h.announcements.size() == 1);
    CHECK(h.announcements.front().find("plays") == 0);
    CHECK(h.announcements.front().find("Session Test - Fixtures") != std::string::npos);

    // The clock moves, on the session's tick.
    REQUIRE(h.loop.waitFor(h.session, [&] { return h.lastPosition > 0.2; },
                           std::chrono::seconds(10)));

    // And the end of the track is a stop: nothing current, nothing playing.
    REQUIRE(h.loop.waitFor(
        h.session,
        [&] { return h.session.currentTrack() == kInvalidTrackId && !h.session.playback().playing(); },
        std::chrono::seconds(20)));
    CHECK(h.tracks.back().id == kInvalidTrackId);
    CHECK(h.settings.LastPlaybackStatus() == 0);
}

TEST_CASE("a lone track under repeat-all comes round as one listen, looping",
          "[uicore][session]") {
    if (!test::haveTool("flac")) {
        SKIP("flac encoder not on PATH");
    }
    const auto one = makeFlac("loops", 0.6, 440.0);
    REQUIRE(one);

    Harness h;
    h.settings.setNotificationsEnable(true);
    h.settings.setRepeatMode(static_cast<int>(RepeatMode::All));
    h.session.settingChanged("repeat");
    h.add({*one});
    const TrackId id = h.session.playlist().entries().front().id;

    h.session.playback().playTrack(id);
    // The seam brings the same entry round: the second announcement of it is
    // flagged as looping, which is what stops an overnight loop counting as a
    // play a minute -- and the notification does not repeat.
    REQUIRE(h.loop.waitFor(
        h.session,
        [&] {
            int seen = 0;
            for (const auto& event : h.tracks) {
                if (event.id == id) {
                    ++seen;
                }
            }
            return seen >= 2;
        },
        std::chrono::seconds(20)));
    bool sawLoop = false;
    for (const auto& event : h.tracks) {
        sawLoop = sawLoop || (event.id == id && event.looping);
    }
    CHECK(sawLoop);
    CHECK(h.announcements.size() == 1);
    h.session.playback().stop();
    h.loop.waitFor(h.session, [&] { return !h.session.playback().playing(); },
                   std::chrono::seconds(5));
}

TEST_CASE("selection follows playback through revealRequested", "[uicore][session]") {
    if (!test::haveTool("flac")) {
        SKIP("flac encoder not on PATH");
    }
    const auto one = makeFlac("follow", 0.5, 440.0);
    REQUIRE(one);

    Harness h;
    h.settings.setSelectionFollowsPlayback(true);
    h.add({*one});
    const TrackId id = h.session.playlist().entries().front().id;

    h.session.playback().playTrack(id);
    REQUIRE(h.loop.waitFor(h.session, [&] { return !h.revealed.empty(); },
                           std::chrono::seconds(10)));
    CHECK(h.revealed.front() == id);
    h.session.playback().stop();
    h.loop.waitFor(h.session, [&] { return !h.session.playback().playing(); },
                   std::chrono::seconds(5));
}

TEST_CASE("setting effects reach the playlist and the frontend", "[uicore][session]") {
    Harness h;

    h.settings.setRepeatMode(static_cast<int>(RepeatMode::All));
    h.session.settingChanged("repeat");
    CHECK(h.session.playlist().repeat() == RepeatMode::All);
    REQUIRE(!h.effects.empty());
    CHECK(h.effects.back() == app::Effect::PlaylistMode);

    // The raw volume row: the engine and the slider both follow.
    h.settings.setVolume(0.25);
    h.session.settingChanged("volume");
    REQUIRE(!h.volumes.empty());
    CHECK(h.volumes.back() == 0.25);
    CHECK(h.effects.back() == app::Effect::Volume);

    // An unknown key is None, and is still published so nothing is lost.
    h.session.settingChanged("xpcog.gtk.window.geometry");
    CHECK(h.effects.back() == app::Effect::None);
}

TEST_CASE("the session leaves the playlist and the position for the next one",
          "[uicore][session]") {
    if (!test::haveTool("flac")) {
        SKIP("flac encoder not on PATH");
    }
    const auto one = makeFlac("persist", 2.0, 440.0);
    REQUIRE(one);

    std::filesystem::path data;
    {
        Harness h;
        data = h.data;
        h.add({*one});
        const TrackId id = h.session.playlist().entries().front().id;
        h.session.playback().playTrack(id);
        REQUIRE(h.loop.waitFor(h.session, [&] { return h.lastPosition > 0.3; },
                               std::chrono::seconds(10)));
        h.session.save();
        // A position, and a playlist row, in the store and the library.
        const std::string position = h.settings.rawValue("xpcog.playback.position");
        CHECK(!position.empty());
        CHECK(std::stod(position) > 0.3);
        CHECK(h.settings.LastPlaybackStatus() == 1);
        h.session.playback().stop();
        h.loop.waitFor(h.session, [&] { return !h.session.playback().playing(); },
                       std::chrono::seconds(5));
    }

    // The library the first session wrote is a playlist the next one loads.
    {
        Library library;
        REQUIRE(library.open((data / "library.db").string()));
        Playlist saved;
        REQUIRE(library.loadPlaylist(saved));
        REQUIRE(saved.size() == 1);
        CHECK(saved.entries().front().title() == "persist");
    }

    // A second session over the same data directory finds it and, with the
    // last session not stopped, asks for its track to be revealed. A memory
    // store is per session, so the one key start() reads is carried over by
    // hand.
    Session::Options o;
    o.dataDirectory  = data.string();
    o.cacheDirectory = (data / "cache").string();
    o.makeOutput     = [](RingBuffer& ring) { return makeOfflineOutput(ring, 8.0); };
    Loop     loop;
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);
    settings.setLastPlaybackStatus(1);
    Session              session(registry(), settings, loop.dispatcher(), o);
    std::vector<TrackId> revealed;
    const Subscription   reveal =
        session.revealRequested.connect([&](TrackId id) { revealed.push_back(id); });
    session.start();
    REQUIRE(session.playlist().size() == 1);
    REQUIRE(revealed.size() == 1);
    CHECK(revealed.front() == session.playlist().entries().front().id);
    // Resume is off by default, so nothing starts.
    loop.drain();
    CHECK_FALSE(session.playback().playing());
}
