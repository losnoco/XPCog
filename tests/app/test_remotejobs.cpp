// Scans started over the API, and what a client polling one is told.
//
// The states matter more than they look. "queued" is not a placeholder: the
// window runs one scan at a time because the PluginCache is not synchronised, so
// a job really can sit waiting for the one before it, and a client watching a
// bar move would otherwise be shown a scan that has not started.

#include "RemoteJobs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace xpcog;
using namespace xpcog::app;

TEST_CASE("a new job is queued and findable", "[remote]") {
    RemoteJobs        jobs;
    const std::string id = jobs.start();

    REQUIRE_FALSE(id.empty());
    const auto job = jobs.find(id);
    REQUIRE(job.has_value());
    CHECK(job->state == "queued");
    CHECK(job->done == 0);
}

TEST_CASE("ids do not repeat", "[remote]") {
    RemoteJobs jobs;
    CHECK(jobs.start() != jobs.start());
}

TEST_CASE("an unknown id is not found rather than invented", "[remote]") {
    RemoteJobs jobs;
    CHECK_FALSE(jobs.find("scan-999").has_value());
    CHECK_FALSE(jobs.find("").has_value());
}

TEST_CASE("a job runs, then finishes at its total", "[remote]") {
    RemoteJobs        jobs;
    const std::string id = jobs.start();

    jobs.setRunning(id, 41, 900);
    auto running = jobs.find(id);
    REQUIRE(running.has_value());
    CHECK(running->state == "running");
    CHECK(running->done == 41);
    CHECK(running->total == 900);

    jobs.finish(id, 880);
    auto done = jobs.find(id);
    REQUIRE(done.has_value());
    CHECK(done->state == "done");
    CHECK(done->added == 880);
    // Snapped to the total, so a client polling sees 900/900 rather than
    // whatever the last progress report happened to say.
    CHECK(done->done == 900);
}

TEST_CASE("a cancelled scan is a failure with a reason", "[remote]") {
    RemoteJobs        jobs;
    const std::string id = jobs.start();

    jobs.fail(id, "cancelled");
    const auto job = jobs.find(id);
    REQUIRE(job.has_value());
    CHECK(job->state == "failed");
    CHECK(job->error == "cancelled");
}

TEST_CASE("updating a job that has been forgotten does nothing", "[remote]") {
    RemoteJobs jobs;
    // Not a crash and not an invented entry: the record is bounded, so a client
    // polling a very old id has to be answerable.
    jobs.setRunning("scan-999", 1, 2);
    jobs.finish("scan-999", 3);
    jobs.fail("scan-999", "no");
    CHECK_FALSE(jobs.find("scan-999").has_value());
}

TEST_CASE("the record is bounded", "[remote]") {
    RemoteJobs        jobs;
    const std::string first = jobs.start();

    // A peer can start jobs, so an unbounded record of them is a slow leak with
    // a network interface on it. The oldest go.
    for (int i = 0; i < 40; ++i) {
        static_cast<void>(jobs.start());
    }

    CHECK_FALSE(jobs.find(first).has_value());
    // And the recent ones are still there, which is what the bound is for.
    const std::string latest = jobs.start();
    CHECK(jobs.find(latest).has_value());
}
