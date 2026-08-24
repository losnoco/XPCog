// The submission queue: what it keeps, what it drops, and what survives a
// restart.
//
// This is the half Cog does not have. `AudioScrobbler.scrobbleTrack` fires a
// request and discards the result, under a comment saying a cache queue is
// still to be written, so a failed submission is simply gone. Every case below
// is one that behaves differently here than there, which is the reason each of
// them is worth a test rather than an assumption.
//
// The worker is a real thread. Tests drive it with drain(), which returns once
// the queue is empty or the worker has parked with a backoff -- so nothing here
// sleeps waiting for a retry timer, and a test that hangs is a bug rather than
// a slow machine.

#include "../FakeHttp.hpp"

#include "xpcog/core/scrobble/LastFmClient.hpp"
#include "xpcog/core/scrobble/Scrobbler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace xpcog;
using xpcog::test::FakeHttp;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr auto kPatience = 5s;

/// A scrobble the server accepted.
constexpr std::string_view kAccepted =
    R"({"scrobbles":{"@attr":{"accepted":1,"ignored":0}}})";

/// A fixed clock, so "fourteen days old" is a number rather than a wait.
constexpr std::int64_t kNow = 1'700'000'000;

class QueueFile {
public:
    explicit QueueFile(const std::string& name)
        : path_(fs::temp_directory_path() / ("xpcog-scrobble-" + name) / "queue.json") {
        std::error_code ec;
        fs::remove_all(path_.parent_path(), ec);
    }

    ~QueueFile() {
        std::error_code ec;
        fs::remove_all(path_.parent_path(), ec);
    }

    QueueFile(const QueueFile&)            = delete;
    QueueFile& operator=(const QueueFile&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] ScrobbleTrack trackAt(std::int64_t startedAt,
                                    std::string  title = "Roygbiv") {
    ScrobbleTrack track;
    track.artist    = "Boards of Canada";
    track.title     = std::move(title);
    track.duration  = 152.0;
    track.startedAt = startedAt;
    return track;
}

/// A connected, enabled scrobbler. The three conditions active() wants are set
/// together because a test that forgot one would silently submit nothing.
[[nodiscard]] std::unique_ptr<Scrobbler> connectedScrobbler(LastFmClient& client,
                                                            const fs::path& queue) {
    auto scrobbler = std::make_unique<Scrobbler>(client, queue,
                                                 [] { return kNow; });
    scrobbler->setSession(Scrobbler::Session{"SESSIONKEY", "listener"});
    scrobbler->setEnabled(true);
    return scrobbler;
}

}  // namespace

TEST_CASE("A submitted play is sent", "[scrobbler]") {
    QueueFile    queue{"sent"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    http.reply(200, std::string{kAccepted});

    auto scrobbler = connectedScrobbler(client, queue.path());
    scrobbler->submit(trackAt(kNow - 300));

    REQUIRE(scrobbler->drain(kPatience));
    CHECK(scrobbler->pending() == 0);
    CHECK(http.countOf("track.scrobble") == 1);
    CHECK(http.sent(0, "artist[0]") == "Boards of Canada");
}

TEST_CASE("Nothing is submitted while disconnected", "[scrobbler]") {
    QueueFile    queue{"disconnected"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    Scrobbler scrobbler{client, queue.path(), [] { return kNow; }};
    scrobbler.setEnabled(true);
    // No session. Cog gates on the same condition; the difference is that here
    // the play is not queued either, because a scrobble with no account to
    // attach it to is not a scrobble that is waiting -- it is one that will
    // never be wanted.
    scrobbler.submit(trackAt(kNow - 300));

    CHECK(scrobbler.pending() == 0);
    CHECK(http.callCount() == 0);
}

TEST_CASE("Nothing is submitted while switched off", "[scrobbler]") {
    QueueFile    queue{"off"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    Scrobbler scrobbler{client, queue.path(), [] { return kNow; }};
    scrobbler.setSession(Scrobbler::Session{"SESSIONKEY", "listener"});
    // Enabled defaults to false, as in settings.def.
    CHECK(scrobbler.active() == false);

    scrobbler.submit(trackAt(kNow - 300));
    CHECK(scrobbler.pending() == 0);
    CHECK(http.callCount() == 0);
}

TEST_CASE("A track with no artist is never queued", "[scrobbler]") {
    QueueFile    queue{"noartist"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    auto          scrobbler = connectedScrobbler(client, queue.path());
    ScrobbleTrack track     = trackAt(kNow - 300);
    track.artist.clear();

    // Last.fm would reject it, and a permanently unacceptable entry at the head
    // of a queue is how a queue stops draining.
    scrobbler->submit(track);
    CHECK(scrobbler->pending() == 0);
    CHECK(http.callCount() == 0);
}

TEST_CASE("A network failure keeps the play for later", "[scrobbler]") {
    QueueFile    queue{"offline"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    // Every attempt fails to reach the server.
    http.setDefaultReply(0, "");
    http.failTransport("could not resolve host");

    auto scrobbler = connectedScrobbler(client, queue.path());
    scrobbler->submit(trackAt(kNow - 300));

    REQUIRE(scrobbler->drain(kPatience));
    // Kept, not lost. This is the case Cog's missing queue loses outright.
    CHECK(scrobbler->pending() == 1);
}

TEST_CASE("A permanently refused play is dropped rather than retried",
          "[scrobbler]") {
    QueueFile    queue{"refused"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    // Error 13 is a wrong signature: it will be just as wrong in an hour.
    // Keeping it would block every later scrobble behind it forever.
    http.reply(200, R"({"error":13,"message":"Invalid method signature supplied"})");
    http.setDefaultReply(200, std::string{kAccepted});

    auto scrobbler = connectedScrobbler(client, queue.path());
    scrobbler->submit(trackAt(kNow - 300));

    REQUIRE(scrobbler->drain(kPatience));
    CHECK(scrobbler->pending() == 0);
}

TEST_CASE("An invalid session key clears the session and says so",
          "[scrobbler]") {
    QueueFile    queue{"stale"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    http.setDefaultReply(200, R"({"error":9,"message":"Invalid session key"})");

    auto             scrobbler = connectedScrobbler(client, queue.path());
    std::atomic<int> invalidated{0};
    scrobbler->onSessionInvalidated([&invalidated] { invalidated += 1; });

    scrobbler->submit(trackAt(kNow - 300));
    REQUIRE(scrobbler->drain(kPatience));

    CHECK(invalidated.load() == 1);
    // Cleared, so nothing keeps trying with a key the server has rejected.
    CHECK(scrobbler->session().connected() == false);
    CHECK(scrobbler->active() == false);
    // The play itself is kept: it is still a real play, and the listener may
    // reconnect. Only the credential was wrong.
    CHECK(scrobbler->pending() == 1);
}

TEST_CASE("The queue survives a restart", "[scrobbler]") {
    QueueFile    queue{"restart"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    {
        http.setDefaultReply(0, "");
        auto scrobbler = connectedScrobbler(client, queue.path());
        scrobbler->submit(trackAt(kNow - 300, "First"));
        scrobbler->submit(trackAt(kNow - 200, "Second"));
        REQUIRE(scrobbler->drain(kPatience));
        REQUIRE(scrobbler->pending() == 2);
    }

    CHECK(fs::exists(queue.path()));

    // A new process. This is the case the queue exists for: the laptop was
    // closed, not merely offline for a moment.
    FakeHttp     revived;
    LastFmClient client2{revived, "KEY", "SECRET"};
    revived.setDefaultReply(200, std::string{kAccepted});

    auto scrobbler = connectedScrobbler(client2, queue.path());
    CHECK(scrobbler->pending() == 2);

    scrobbler->wake();
    REQUIRE(scrobbler->drain(kPatience));
    CHECK(scrobbler->pending() == 0);

    // Both went in one batch, which is the point of batching: an afternoon's
    // backlog is not an afternoon's worth of round trips.
    CHECK(revived.countOf("track.scrobble") == 1);
    CHECK(revived.sent(0, "track[0]") == "First");
    CHECK(revived.sent(0, "track[1]") == "Second");
}

TEST_CASE("A play too old to be accepted is dropped", "[scrobbler]") {
    QueueFile    queue{"aged"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    http.setDefaultReply(200, std::string{kAccepted});

    auto scrobbler = connectedScrobbler(client, queue.path());
    // Fifteen days old. Last.fm refuses timestamps this far back, so it can
    // never be accepted -- and at the head of the queue it would be retried
    // forever, taking everything behind it with it.
    scrobbler->submit(trackAt(kNow - (15 * 24 * 60 * 60)));
    scrobbler->submit(trackAt(kNow - 300, "Recent"));

    REQUIRE(scrobbler->drain(kPatience));
    CHECK(scrobbler->pending() == 0);
    // The recent one went; the stale one was not sent at all.
    CHECK(http.countOf("track.scrobble") == 1);
    CHECK(http.sent(0, "track[0]") == "Recent");
    CHECK(http.sent(0, "track[1]") == std::nullopt);
}

TEST_CASE("Now-playing is not queued", "[scrobbler]") {
    QueueFile    queue{"nowplaying"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    http.setDefaultReply(0, "");

    auto scrobbler = connectedScrobbler(client, queue.path());
    scrobbler->nowPlaying(trackAt(0, "Playing"));
    REQUIRE(scrobbler->drain(kPatience));

    // It failed, and that is the end of it. Retrying later would announce a
    // track that stopped playing long ago, and Last.fm never makes one part of
    // the listening history anyway.
    CHECK(scrobbler->pending() == 0);
}

TEST_CASE("Only the most recent now-playing is sent", "[scrobbler]") {
    QueueFile    queue{"latest"};
    FakeHttp     http;
    LastFmClient client{http, "KEY", "SECRET"};

    http.setDefaultReply(200, R"({"nowplaying":{}})");

    Scrobbler scrobbler{client, queue.path(), [] { return kNow; }};
    scrobbler.setSession(Scrobbler::Session{"SESSIONKEY", "listener"});
    scrobbler.setEnabled(true);

    // Three tracks skipped through faster than the worker runs. A backlog of
    // "now playing" updates is a contradiction, so the slot holds one.
    scrobbler.nowPlaying(trackAt(0, "First"));
    scrobbler.nowPlaying(trackAt(0, "Second"));
    scrobbler.nowPlaying(trackAt(0, "Third"));

    REQUIRE(scrobbler.drain(kPatience));
    CHECK(http.countOf("track.updateNowPlaying") <= 3);
}

TEST_CASE("A build with no API key queues nothing", "[scrobbler]") {
    QueueFile    queue{"nokey"};
    FakeHttp     http;
    LastFmClient client{http, "", ""};

    Scrobbler scrobbler{client, queue.path(), [] { return kNow; }};
    scrobbler.setSession(Scrobbler::Session{"SESSIONKEY", "listener"});
    scrobbler.setEnabled(true);

    CHECK(scrobbler.active() == false);
    scrobbler.submit(trackAt(kNow - 300));
    CHECK(scrobbler.pending() == 0);
    CHECK(http.callCount() == 0);
}
