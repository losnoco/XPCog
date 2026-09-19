// The LRCLIB protocol, pinned against a scripted transport, and the lookup
// that sits over it.
//
// The client's half is what the server would only tell you about by refusing
// or by answering wrongly: which parameters go, what is left out, which
// header names the program, and which statuses mean "no such track" versus
// "not now". The lookup's half is the promise the header makes -- ask once,
// remember, hand back on the interface thread -- which is the part that
// decides whether the pane is a good citizen of a free service or a
// keypress-driven request generator.

#include "../FakeHttp.hpp"

#include "xpcog/core/Version.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/lyrics/LibraryLyricsStore.hpp"
#include "xpcog/core/lyrics/LrclibClient.hpp"
#include "xpcog/core/lyrics/LyricsLookup.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace xpcog;
using xpcog::test::FakeHttp;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kFound = R"json({
  "id": 3396226,
  "trackName": "I Want to Live",
  "artistName": "Borislav Slavov",
  "albumName": "Baldur's Gate 3 (Original Game Soundtrack)",
  "duration": 233.0,
  "instrumental": false,
  "plainLyrics": "I feel your breath upon my neck\nA soft caress as cold as death",
  "syncedLyrics": "[00:17.12] I feel your breath upon my neck\n[00:20.65] A soft caress as cold as death"
})json";

constexpr std::string_view kInstrumental = R"({
  "id": 1, "trackName": "Interlude", "artistName": "Someone", "albumName": null,
  "duration": 60.0, "instrumental": true, "plainLyrics": null, "syncedLyrics": null
})";

constexpr std::string_view kNotFound =
    R"({"message":"Failed to find specified track","name":"TrackNotFound","statusCode":404})";

[[nodiscard]] LyricsQuery query() {
    LyricsQuery q;
    q.title    = "I Want to Live";
    q.artist   = "Borislav Slavov";
    q.album    = "Baldur's Gate 3 (Original Game Soundtrack)";
    q.duration = 233.4;
    return q;
}

/// A dispatcher that is a queue the test drains where it chooses, and a
/// clock that is a number the test moves. Both thread-safe: the worker calls
/// the dispatcher.
struct Interface {
    std::mutex                        mutex;
    std::deque<std::function<void()>> queue;
    std::int64_t                      now = 1'700'000'000;

    Dispatcher dispatcher() {
        return [this](std::function<void()> task) {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(task));
        };
    }

    LyricsLookup::Clock clock() {
        return [this] {
            std::lock_guard lock(mutex);
            return now;
        };
    }

    void advance(std::int64_t seconds) {
        std::lock_guard lock(mutex);
        now += seconds;
    }

    /// Runs what has been dispatched, waiting up to a couple of seconds for
    /// at least one thing to arrive first: the worker is a real thread.
    void drain() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex);
                if (!queue.empty()) {
                    task = std::move(queue.front());
                    queue.pop_front();
                }
            }
            if (task) {
                task();
                std::lock_guard lock(mutex);
                if (queue.empty()) {
                    return;
                }
                continue;
            }
            std::this_thread::sleep_for(5ms);
        }
        FAIL("nothing was dispatched");
    }
};

/// A store that is a map, and counts.
class MapStore final : public ILyricsStore {
public:
    std::optional<StoredLyrics> load(std::string_view key) const override {
        ++loads;
        const auto it = rows.find(std::string{key});
        return it == rows.end() ? std::nullopt : std::optional{it->second};
    }
    bool store(std::string_view key, const StoredLyrics& record) override {
        ++stores;
        rows[std::string{key}] = record;
        return true;
    }

    std::map<std::string, StoredLyrics> rows;
    mutable int                         loads  = 0;
    int                                 stores = 0;
};

}  // namespace

// --- the client -----------------------------------------------------------

TEST_CASE("A lookup is one GET to /api/get with the track named", "[lrclib]") {
    FakeHttp     http;
    LrclibClient client{http};
    http.reply(200, std::string{kFound});

    const auto lyrics = client.get(query());
    REQUIRE(lyrics);
    REQUIRE(http.callCount() == 1);
    const auto call = http.calls()[0];
    CHECK_FALSE(call.post);
    CHECK(call.url == "https://lrclib.net/api/get");
    CHECK(http.sent(0, "track_name") == "I Want to Live");
    CHECK(http.sent(0, "artist_name") == "Borislav Slavov");
    CHECK(http.sent(0, "album_name") == "Baldur's Gate 3 (Original Game Soundtrack)");
    // Whole seconds: the server matches within two and rounds for its own
    // cache, so decimals buy nothing.
    CHECK(http.sent(0, "duration") == "233");

    CHECK(lyrics->plain == "I feel your breath upon my neck\nA soft caress as cold as death");
    CHECK(lyrics->synced.starts_with("[00:17.12]"));
    CHECK_FALSE(lyrics->instrumental);
}

TEST_CASE("Every request names the program in Lrclib-Client", "[lrclib]") {
    FakeHttp     http;
    LrclibClient client{http};
    http.reply(200, std::string{kFound});

    static_cast<void>(client.get(query()));
    REQUIRE(http.callCount() == 1);
    const auto header = http.header(0, "Lrclib-Client");
    REQUIRE(header);
    // The one User-Agent, under the name the server reads first.
    CHECK(*header == LrclibClient::clientHeader());
    CHECK(*header == userAgent());
    CHECK(header->starts_with("XPCog/" + std::string{kVersionString}));
    CHECK(header->find(kProjectUrl) != std::string::npos);
}

TEST_CASE("An unknown or absurd length is left out rather than sent", "[lrclib]") {
    FakeHttp     http;
    LrclibClient client{http};
    http.setDefaultReply(200, std::string{kFound});

    LyricsQuery q = query();
    q.duration    = 0.0;  // a live stream
    static_cast<void>(client.get(q));
    CHECK_FALSE(http.sent(0, "duration"));

    q.duration = 0.3;  // rounds to zero, which the server refuses outright
    static_cast<void>(client.get(q));
    CHECK_FALSE(http.sent(1, "duration"));

    q.duration = 4000.0;  // past the hour the server validates against
    static_cast<void>(client.get(q));
    CHECK_FALSE(http.sent(2, "duration"));

    q.duration = 1.0;
    static_cast<void>(client.get(q));
    CHECK(http.sent(3, "duration") == "1");
}

TEST_CASE("An empty album is not sent as an empty parameter", "[lrclib]") {
    FakeHttp     http;
    LrclibClient client{http};
    http.reply(200, std::string{kFound});

    LyricsQuery q = query();
    q.album.clear();
    static_cast<void>(client.get(q));
    CHECK_FALSE(http.sent(0, "album_name"));
}

TEST_CASE("A query with no title or no artist is refused before it is sent", "[lrclib]") {
    FakeHttp     http;
    LrclibClient client{http};

    LyricsQuery q = query();
    q.artist.clear();
    LyricsError error;
    CHECK_FALSE(client.get(q, &error));
    CHECK(error.kind == LyricsError::Kind::Incomplete);

    q        = query();
    q.title  = "";
    CHECK_FALSE(client.get(q, &error));
    CHECK(error.kind == LyricsError::Kind::Incomplete);

    CHECK(http.callCount() == 0);
}

TEST_CASE("404 is 'no such track', not a failure", "[lrclib]") {
    FakeHttp     http;
    LrclibClient client{http};
    http.reply(404, std::string{kNotFound});

    LyricsError error;
    CHECK_FALSE(client.get(query(), &error));
    CHECK(error.kind == LyricsError::Kind::NotFound);
    CHECK(error.code == 404);
    CHECK(error.message == "Failed to find specified track");
}

TEST_CASE("An instrumental comes back with nulls for both texts", "[lrclib]") {
    FakeHttp     http;
    LrclibClient client{http};
    http.reply(200, std::string{kInstrumental});

    const auto lyrics = client.get(query());
    REQUIRE(lyrics);
    CHECK(lyrics->instrumental);
    CHECK(lyrics->plain.empty());
    CHECK(lyrics->synced.empty());
}

TEST_CASE("Statuses sort into transport, transient, api and malformed", "[lrclib]") {
    FakeHttp     http;
    LrclibClient client{http};
    LyricsError  error;

    http.failTransport("could not resolve host");
    CHECK_FALSE(client.get(query(), &error));
    CHECK(error.kind == LyricsError::Kind::Transport);
    CHECK(error.message == "could not resolve host");

    http.reply(503, R"({"message":"The server is busy, please retry in a moment",)"
                    R"("name":"ServerOverloaded","statusCode":503})");
    CHECK_FALSE(client.get(query(), &error));
    CHECK(error.kind == LyricsError::Kind::Transient);
    CHECK(error.code == 503);

    http.reply(429, "");
    CHECK_FALSE(client.get(query(), &error));
    CHECK(error.kind == LyricsError::Kind::Transient);

    http.reply(400, R"({"message":"duration: must be between 1 and 3600",)"
                    R"("name":"ValidationError","statusCode":400})");
    CHECK_FALSE(client.get(query(), &error));
    CHECK(error.kind == LyricsError::Kind::Api);
    CHECK(error.message == "duration: must be between 1 and 3600");

    http.reply(200, "<html>not json</html>");
    CHECK_FALSE(client.get(query(), &error));
    CHECK(error.kind == LyricsError::Kind::Malformed);
}

TEST_CASE("The root is accepted with or without /api and a trailing slash", "[lrclib]") {
    FakeHttp http;
    http.setDefaultReply(200, std::string{kFound});

    for (const char* root : {"https://lrclib.example/", "https://lrclib.example/api",
                             "https://lrclib.example/api/", "https://lrclib.example"}) {
        LrclibClient client{http, root};
        static_cast<void>(client.get(query()));
        CHECK(http.calls().back().url == "https://lrclib.example/api/get");
    }
}

// --- the lookup -----------------------------------------------------------

TEST_CASE("The lookup answers on the interface thread and asks once", "[lrclib][lookup]") {
    FakeHttp  http;
    Interface ui;
    MapStore  store;
    http.reply(200, std::string{kFound});

    LyricsLookup lookup{http, ui.dispatcher(), &store, std::string{LrclibClient::kDefaultApiRoot},
                        ui.clock()};

    std::vector<LyricsLookup::Answer> answers;
    lookup.lookup(query(), [&](const LyricsLookup::Answer& a) { answers.push_back(a); });
    CHECK(lookup.pending(query()));
    CHECK(answers.empty());  // nothing before the interface runs what was dispatched

    ui.drain();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].outcome == LyricsLookup::Outcome::Found);
    CHECK(answers[0].lyrics.starts_with("I feel your breath"));
    CHECK(answers[0].synced.starts_with("[00:17.12]"));
    CHECK_FALSE(lookup.pending(query()));

    // The second time is from memory: still through the dispatcher, but no
    // request.
    lookup.lookup(query(), [&](const LyricsLookup::Answer& a) { answers.push_back(a); });
    ui.drain();
    REQUIRE(answers.size() == 2);
    CHECK(answers[1].outcome == LyricsLookup::Outcome::Found);
    CHECK(http.callCount() == 1);

    // And cached() says so without dispatching anything.
    const auto known = lookup.cached(query());
    REQUIRE(known);
    CHECK(known->lyrics == answers[0].lyrics);
}

TEST_CASE("The same question asked twice while in flight gets one request", "[lrclib][lookup]") {
    FakeHttp  http;
    Interface ui;
    http.reply(200, std::string{kFound});

    LyricsLookup lookup{http, ui.dispatcher(), nullptr, std::string{LrclibClient::kDefaultApiRoot},
                        ui.clock()};

    int heard = 0;
    lookup.lookup(query(), [&](const LyricsLookup::Answer&) { ++heard; });
    lookup.lookup(query(), [&](const LyricsLookup::Answer&) { ++heard; });
    ui.drain();

    CHECK(heard == 2);
    CHECK(http.callCount() == 1);
}

TEST_CASE("Answers go to the store, failures do not", "[lrclib][lookup]") {
    FakeHttp  http;
    Interface ui;
    MapStore  store;

    LyricsLookup lookup{http, ui.dispatcher(), &store, std::string{LrclibClient::kDefaultApiRoot},
                        ui.clock()};

    http.reply(200, std::string{kFound});
    lookup.lookup(query(), [](const LyricsLookup::Answer&) {});
    ui.drain();
    REQUIRE(store.stores == 1);
    const StoredLyrics& row = store.rows.begin()->second;
    CHECK(row.known);
    CHECK_FALSE(row.instrumental);
    CHECK(row.plain.starts_with("I feel your breath"));
    CHECK(row.synced.starts_with("[00:17.12]"));
    CHECK(row.fetchedAt == 1'700'000'000);

    LyricsQuery missing = query();
    missing.title       = "Something Else";
    http.reply(404, std::string{kNotFound});
    lookup.lookup(missing, [](const LyricsLookup::Answer&) {});
    ui.drain();
    CHECK(store.stores == 2);
    CHECK_FALSE(store.rows[LyricsLookup::keyOf(missing)].known);

    LyricsQuery offline = query();
    offline.title       = "Unreachable";
    http.failTransport("no network");
    LyricsLookup::Answer failed;
    lookup.lookup(offline, [&](const LyricsLookup::Answer& a) { failed = a; });
    ui.drain();
    CHECK(failed.outcome == LyricsLookup::Outcome::Failed);
    CHECK(failed.error.kind == LyricsError::Kind::Transport);
    CHECK(store.stores == 2);  // unchanged
}

TEST_CASE("A stored answer is served without a request, across a relaunch", "[lrclib][lookup]") {
    FakeHttp  http;
    Interface ui;
    MapStore  store;

    {
        http.reply(200, std::string{kFound});
        LyricsLookup first{http, ui.dispatcher(), &store,
                           std::string{LrclibClient::kDefaultApiRoot}, ui.clock()};
        first.lookup(query(), [](const LyricsLookup::Answer&) {});
        ui.drain();
    }
    REQUIRE(http.callCount() == 1);

    // A new lookup with an empty memory but the same store: the next session.
    LyricsLookup second{http, ui.dispatcher(), &store, std::string{LrclibClient::kDefaultApiRoot},
                        ui.clock()};
    const auto known = second.cached(query());
    REQUIRE(known);
    CHECK(known->outcome == LyricsLookup::Outcome::Found);
    CHECK(known->lyrics.starts_with("I feel your breath"));
    CHECK(http.callCount() == 1);

    // Read from the store once; after that it is in memory.
    const int loads = store.loads;
    static_cast<void>(second.cached(query()));
    CHECK(store.loads == loads);
}

TEST_CASE("Not found is remembered for a week, a failure for a minute", "[lrclib][lookup]") {
    FakeHttp  http;
    Interface ui;
    MapStore  store;

    LyricsLookup lookup{http, ui.dispatcher(), &store, std::string{LrclibClient::kDefaultApiRoot},
                        ui.clock()};

    http.reply(404, std::string{kNotFound});
    lookup.lookup(query(), [](const LyricsLookup::Answer&) {});
    ui.drain();
    REQUIRE(http.callCount() == 1);

    ui.advance(LyricsLookup::kNotFoundMemory - 1);
    CHECK(lookup.cached(query()));
    ui.advance(2);
    CHECK_FALSE(lookup.cached(query()));

    // Asked again, and this time the service has it: the row changes its
    // mind rather than sitting beside the old answer.
    http.reply(200, std::string{kFound});
    lookup.lookup(query(), [](const LyricsLookup::Answer&) {});
    ui.drain();
    CHECK(http.callCount() == 2);
    CHECK(store.rows.size() == 1);
    CHECK(store.rows.begin()->second.known);

    LyricsQuery offline = query();
    offline.title       = "Unreachable";
    http.failTransport("no network");
    lookup.lookup(offline, [](const LyricsLookup::Answer&) {});
    ui.drain();
    REQUIRE(http.callCount() == 3);

    // Within the minute the failure is what cached() answers, so the pane
    // does not queue a timeout per keypress.
    const auto held = lookup.cached(offline);
    REQUIRE(held);
    CHECK(held->outcome == LyricsLookup::Outcome::Failed);

    ui.advance(LyricsLookup::kFailureMemory);
    CHECK_FALSE(lookup.cached(offline));
    http.reply(200, std::string{kFound});
    lookup.lookup(offline, [](const LyricsLookup::Answer&) {});
    ui.drain();
    CHECK(http.callCount() == 4);
}

TEST_CASE("A stale 'not found' in the store is not served", "[lrclib][lookup]") {
    FakeHttp  http;
    Interface ui;
    MapStore  store;

    StoredLyrics old;
    old.known     = false;
    old.fetchedAt = ui.now - LyricsLookup::kNotFoundMemory - 1;
    store.rows[LyricsLookup::keyOf(query())] = old;

    LyricsLookup lookup{http, ui.dispatcher(), &store, std::string{LrclibClient::kDefaultApiRoot},
                        ui.clock()};
    CHECK_FALSE(lookup.cached(query()));

    // While a hit of any age is.
    StoredLyrics hit;
    hit.known     = true;
    hit.plain     = "words";
    hit.fetchedAt = 0;
    store.rows[LyricsLookup::keyOf(query())] = hit;
    LyricsLookup fresh{http, ui.dispatcher(), &store, std::string{LrclibClient::kDefaultApiRoot},
                       ui.clock()};
    const auto known = fresh.cached(query());
    REQUIRE(known);
    CHECK(known->lyrics == "words");
}

TEST_CASE("Changing the server forgets the session's answers but not the store's",
          "[lrclib][lookup]") {
    FakeHttp  http;
    Interface ui;
    MapStore  store;
    http.setDefaultReply(200, std::string{kFound});

    LyricsLookup lookup{http, ui.dispatcher(), &store, std::string{LrclibClient::kDefaultApiRoot},
                        ui.clock()};
    lookup.lookup(query(), [](const LyricsLookup::Answer&) {});
    ui.drain();
    REQUIRE(http.callCount() == 1);

    lookup.setApiRoot("https://mirror.example");
    CHECK(lookup.apiRoot() == "https://mirror.example");
    // Memory is gone, the store is not, so the answer is still known.
    CHECK(lookup.cached(query()));

    LyricsQuery other = query();
    other.title       = "Another";
    lookup.lookup(other, [](const LyricsLookup::Answer&) {});
    ui.drain();
    REQUIRE(http.callCount() == 2);
    CHECK(http.calls()[1].url == "https://mirror.example/api/get");
}

TEST_CASE("An incomplete query is answered Failed without a request", "[lrclib][lookup]") {
    FakeHttp  http;
    Interface ui;

    LyricsLookup lookup{http, ui.dispatcher(), nullptr, std::string{LrclibClient::kDefaultApiRoot},
                        ui.clock()};
    LyricsQuery q = query();
    q.artist.clear();

    LyricsLookup::Answer answer;
    lookup.lookup(q, [&](const LyricsLookup::Answer& a) { answer = a; });
    ui.drain();
    CHECK(answer.outcome == LyricsLookup::Outcome::Failed);
    CHECK(answer.error.kind == LyricsError::Kind::Incomplete);
    CHECK(http.callCount() == 0);
}

TEST_CASE("The key rounds the length the way the request does", "[lrclib][lookup]") {
    LyricsQuery a = query();
    LyricsQuery b = query();
    a.duration    = 233.4;
    b.duration    = 232.6;  // the same whole second
    CHECK(LyricsLookup::keyOf(a) == LyricsLookup::keyOf(b));
    b.duration = 231.4;
    CHECK(LyricsLookup::keyOf(a) != LyricsLookup::keyOf(b));
}

// --- the library as the store ---------------------------------------------

TEST_CASE("The library keeps a lyrics answer and reads it back", "[lrclib][library]") {
    Library library;
    REQUIRE(library.open(":memory:"));
    LibraryLyricsStore store{library};

    CHECK_FALSE(store.load("nobody"));

    StoredLyrics record;
    record.known        = true;
    record.instrumental = false;
    record.plain        = "line one\nline two";
    record.synced       = "[00:01.00] line one\n[00:02.00] line two";
    record.fetchedAt    = 1'700'000'000;
    const std::string key = LyricsLookup::keyOf(query());
    REQUIRE(store.store(key, record));

    const auto back = store.load(key);
    REQUIRE(back);
    CHECK(back->known);
    CHECK_FALSE(back->instrumental);
    CHECK(back->plain == record.plain);
    CHECK(back->synced == record.synced);
    CHECK(back->fetchedAt == record.fetchedAt);

    // Replaced, not duplicated.
    record.known = false;
    record.plain.clear();
    REQUIRE(store.store(key, record));
    const auto again = store.load(key);
    REQUIRE(again);
    CHECK_FALSE(again->known);
    CHECK(again->plain.empty());
}
