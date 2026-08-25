#!/usr/bin/env bash
#
# Watch a GitHub Actions run and print one line per job as it finishes.
#
# Written for the Monitor tool, whose contract is that every stdout line is an
# event: the caller keeps working and each line arrives as a notification. That
# is why this prints deltas rather than a table -- reprinting the same status
# every poll would be a notification storm, and printing only at the end would
# be a blocking wait wearing a different hat.
#
#   tools/ci-watch/ci-watch.sh                 # the run for HEAD
#   tools/ci-watch/ci-watch.sh 32260674631     # a run by id
#
# Every line is a job reaching a terminal state, or the run itself finishing:
#
#   Headless (no toolkit): success
#   macOS: success
#   Linux: failure (Test)
#   Windows: success
#   RUN COMPLETE: failure  https://github.com/losnoco/XPCog/actions/runs/32260674631
#
# A failing job names the step that failed, because with four jobs and a dozen
# steps each, "which one, and how far did it get" is the whole question.
#
# Exit status is the run's: 0 for success, 1 for anything else, so this also
# works as `... && echo ok` outside a monitor.
#
# Environment: CI_WATCH_POLL_SECONDS (default 30), CI_WATCH_MAX_POLLS (120).

set -uo pipefail

readonly POLL_SECONDS="${CI_WATCH_POLL_SECONDS:-30}"
readonly MAX_POLLS="${CI_WATCH_MAX_POLLS:-120}"  # 60 minutes at the default

usage() {
    sed -n '3,28p' "$0" | sed 's|^#\s\?||'
    exit 2
}

case "${1:-}" in
    -h|--help) usage ;;
esac

command -v gh >/dev/null || { echo "ci-watch: gh is not installed" >&2; exit 2; }

run_id="${1:-}"

# No id given: the run for the commit that is checked out, not merely the latest
# on the branch. Immediately after a push those differ -- the run for the new
# commit does not exist for a few seconds, and latching onto its predecessor
# would report a finished run as though it were this one.
if [[ -z "$run_id" ]]; then
    sha=$(git rev-parse HEAD 2>/dev/null) || {
        echo "ci-watch: not a git repository, and no run id given" >&2
        exit 2
    }
    for _ in $(seq 1 20); do
        run_id=$(gh run list --commit "$sha" --limit 1 --json databaseId \
                     --jq '.[0].databaseId' 2>/dev/null) || run_id=""
        [[ -n "$run_id" ]] && break
        sleep 5
    done
    if [[ -z "$run_id" ]]; then
        echo "ci-watch: no run for ${sha:0:8} after 100s -- pushed yet?" >&2
        exit 2
    fi
fi

# gh's own --jq rather than a pipe into jq: gh embeds the filter engine, and a
# standalone jq is not on a stock Windows box -- which is where this port is
# developed. One request per poll, emitting both the finished jobs and the run's
# own state, so the two cannot disagree by a poll interval.
#
# The step named for a non-successful job is the first that did not succeed:
# the one that failed, or for a cancellation the one it was interrupted in.
readonly FILTER='
    ( .jobs[]
      | select(.status == "completed")
      | [ "JOB"
        , .name
        , .conclusion
        , ( [ .steps[]
              | select(.conclusion != "success" and .conclusion != "skipped"
                       and .conclusion != null)
            ][0].name // ""
          )
        ] | @tsv
    ),
    ([ "RUN", .status, (.conclusion // ""), .url ] | @tsv)
'

# Jobs already reported, so each is announced once. Newline-delimited and matched
# whole-line, since one job name can be a prefix of another.
reported=$'\n'

for _ in $(seq 1 "$MAX_POLLS"); do
    # A failed request is not fatal: a dropped connection or a rate-limit blip
    # should cost a cycle, not the whole watch.
    if ! view=$(gh run view "$run_id" --json status,conclusion,url,jobs \
                    --jq "$FILTER" 2>/dev/null); then
        sleep "$POLL_SECONDS"
        continue
    fi

    run_status=""
    run_conclusion=""
    run_url=""
    while IFS=$'\t' read -r kind a b c; do
        case "$kind" in
            JOB)
                [[ -z "$a" ]] && continue
                [[ "$reported" == *$'\n'"$a"$'\n'* ]] && continue
                reported+="$a"$'\n'
                if [[ "$b" == "success" || -z "$c" ]]; then
                    echo "$a: $b"
                else
                    echo "$a: $b ($c)"
                fi
                ;;
            RUN)
                run_status="$a"
                run_conclusion="$b"
                run_url="$c"
                ;;
        esac
    done <<<"$view"

    if [[ "$run_status" == "completed" ]]; then
        echo "RUN COMPLETE: $run_conclusion  $run_url"
        [[ "$run_conclusion" == "success" ]] && exit 0
        exit 1
    fi

    sleep "$POLL_SECONDS"
done

echo "ci-watch: gave up after $((MAX_POLLS * POLL_SECONDS))s; run $run_id still going"
exit 1
