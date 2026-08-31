# release-notes

The changelog on a GitHub release, built from the commits between the previous
`v` tag and the one being published.

```sh
GH_REPO=losnoco/XPCog tools/release-notes/release-notes.sh v1.3.0 v1.4.0
```

## Why it exists

The release job used to say `gh release create --generate-notes`. That generator
lists **merged pull requests**, and this repository commits straight to main — so
the notes said what you could download and nothing at all about what had changed.

## Two details worth keeping

**It reads the API, not a checkout.** The release job's own header explains that
it deliberately has no checkout: everything it needs is an artifact from another
job or an answer from the API. A changelog is another answer from the API, so
this uses the `compare` endpoint rather than `git log`. The job now does a sparse
checkout of this directory alone — one file, no history — which is not the same
thing as fetching the repository to run git in it.

**It parses with `gh --jq`, not a standalone `jq`,** for the reason
`tools/ci-watch` gives: `jq` is not on a stock Windows box and `gh` carries its
own.

## What it leaves out

Commits marked `[skip ci]` — the AUR package bumps — and the version bump
itself. Both are in the history for anyone who wants them and say nothing to
somebody deciding whether to upgrade.

Bodies are not reproduced. The commit convention already puts a plain
present-tense summary on the headline and everything else underneath, so the
headline is the entry; the body is for whoever is reading the history.

## What it does not do

It does not group entries by area. Doing that means knowing which paths each
commit touched, and the compare endpoint does not carry them — it would be one
extra API call per commit, for a list whose headlines already say what they
touched. If that changes, `repos/{owner}/{repo}/commits/{sha}` is where the file
list is.
