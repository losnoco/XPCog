# CI watch

Watches a GitHub Actions run and prints one line per job as it finishes.

```sh
tools/ci-watch/ci-watch.sh                 # the run for HEAD
tools/ci-watch/ci-watch.sh 32260674631     # a run by id
```

```
Headless (no Qt): success
macOS: success
Linux: failure (Test)
Windows: success
RUN COMPLETE: failure  https://github.com/losnoco/XPCog/actions/runs/32260674631
```

Requires `gh`, authenticated. Exits 0 when the run succeeded and 1 otherwise, so
it also reads as a plain command outside anything that watches it.

## Why a script rather than a poll loop

Four jobs, and the slow one takes five minutes on a cache hit and twenty on a
miss. Sitting on `gh run watch` blocks the terminal for all of it; re-running
`gh run list` by hand turns a twenty-minute wait into twenty context switches
that each learn nothing.

The shape that works is one line per event, on stdout, as it happens. An agent
harness can then run this as a background monitor and be notified per job while
it does something else — every line becomes a message, and the stream ends by
itself when the run does. Nothing here is specific to that; a person can run it
in a spare terminal and get the same four lines.

A failing job names the step it failed in. That one word — `Build` against
`Test` — is the difference between "the compiler rejected something" and
"the compiler was fine and behaviour is wrong", and it is the first thing worth
knowing before opening a log that runs to five figures of lines.

## Notes

`gh`'s built-in `--jq` does the parsing rather than a pipe into `jq`. A
standalone `jq` is not on a stock Windows box, and this port is developed on
one. One request per poll returns the finished jobs and the run's own state
together, so the two cannot disagree by a poll interval.

With no argument it resolves the run for the **checked-out commit**, not the
latest run on the branch, and waits up to 100 seconds for that run to appear.
Straight after a push those differ: latching onto the previous commit's run
would report a finished, possibly green run as though it were this one.

`CI_WATCH_POLL_SECONDS` (default 30) and `CI_WATCH_MAX_POLLS` (default 120,
so an hour) tune the polling. Thirty seconds is deliberate rather than
conservative — GitHub's API is rate-limited per hour, and nothing here changes
often enough to be worth a tighter loop.
