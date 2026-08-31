#!/bin/sh
# The changelog for a release, from the commits between two points.
#
# Written because the release job's notes were close to empty. `gh release
# create --generate-notes` lists *merged pull requests*, and this repository
# commits straight to main, so what it produced said what you could download and
# nothing about what had changed.
#
# Read through the API rather than from a checkout. The release job states that
# it deliberately has none -- "everything this job needs is either an artifact
# from another job or an answer from the API" -- and a changelog is another
# answer from the API. Adding actions/checkout with fetch-depth: 0 just to run
# `git log` would give that property up for nothing.
#
# The commit convention does the formatting. Headlines here are already one
# plain present-tense line of about 72 characters, with everything else in the
# body, so an entry is the headline and a link. Bodies are not reproduced: they
# are for whoever is reading the history, not for a release page.
#
# Usage:
#   release-notes.sh <base-ref> <head-ref>
#
# Writes Markdown to stdout. Needs `gh` authenticated and GH_REPO set, or a
# checkout `gh` can infer the repository from.
#
# Try it against two existing tags before changing it:
#   GH_REPO=losnoco/XPCog tools/release-notes/release-notes.sh v1.3.0 v1.4.0

set -eu

if [ $# -ne 2 ]; then
    echo "usage: release-notes.sh <base-ref> <head-ref>" >&2
    exit 2
fi

base=$1
head=$2

# --jq rather than a standalone jq, for the reason tools/ci-watch gives: jq is
# not on a stock Windows box and gh carries its own.
#
# Tab-separated so the shell can split it, and the message truncated to its first
# line here rather than in the loop -- a body containing a tab would otherwise
# derail the read.
commits=$(gh api --paginate \
    "repos/{owner}/{repo}/compare/${base}...${head}" \
    --jq '.commits[] | [.sha, (.commit.message | split("\n")[0])] | @tsv' 2>/dev/null || true)

if [ -z "$commits" ]; then
    # Not an error. A release with nothing between it and the last one is
    # unusual but legitimate -- a re-tag, or a first release -- and a heading
    # with nothing under it reads as a bug in this script.
    echo "No changes recorded."
    exit 0
fi

server=${GITHUB_SERVER_URL:-https://github.com}
repo=${GH_REPO:-${GITHUB_REPOSITORY:-}}

printf '### Changes\n\n'

printf '%s\n' "$commits" | while IFS='	' read -r sha headline; do
    [ -n "$sha" ] || continue

    # The version bump and the AUR bumps are bookkeeping. They are in the history
    # for whoever wants it and say nothing to somebody deciding whether to
    # upgrade.
    case "$headline" in
        *'[skip ci]'*) continue ;;
    esac
    case "$headline" in
        'Bump the version'*|'Release '*) continue ;;
    esac

    short=$(printf '%s' "$sha" | cut -c1-7)
    if [ -n "$repo" ]; then
        printf -- '- %s ([%s](%s/%s/commit/%s))\n' "$headline" "$short" \
            "$server" "$repo" "$sha"
    else
        printf -- '- %s (%s)\n' "$headline" "$short"
    fi
done
