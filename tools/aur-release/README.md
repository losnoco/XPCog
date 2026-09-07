# aur-release

Takes a published XPCog release to the [AUR](https://aur.archlinux.org/packages/xpcog).

The AUR is a second publication of the same release and nothing in CI does it.
The tarball the package names does not exist until the release job has run, so
this cannot be folded into the build — it is a thing done afterwards, by hand,
and this script is the sequence rather than a new policy.

```sh
tools/aur-release/aur-release.sh prepare          # the tree's own version
tools/aur-release/aur-release.sh prepare 1.8.1    # or a named one
# review, then commit: "Update the AUR package to 1.8.1 [skip ci]"
tools/aur-release/aur-release.sh publish -m "Fix ..."
```

## What `prepare` does that is easy to get wrong

**It refuses to run before the release exists.** The first step is
`gh release view`, because the tarball being checksummed is published by CI's
release job; running any earlier hashes a 404 and the mistake surfaces days
later as a package nobody can install.

**It writes the checksum by hand.** `updpkgsums` rewrites the whole
`sha256sums=()` array in its own single-line layout and replaces the vcpkg
entry's `SKIP` with a hash of the git tree it happened to fetch — which is not
reproducible, and contradicts the comment above the array saying a git source is
pinned by its commit. This replaces the one line and then checks that `SKIP`
survived.

**It checks the pin against the released tree, not the working one.** The
PKGBUILD's `prepare()` compares `_vcpkg_commit` against the `builtin-baseline`
inside the tarball; a working tree that has moved on would agree with itself and
still fail every user's build. So the baseline is read out of the downloaded
archive.

**It resets `pkgrel` to 1**, which counts packaging revisions within one upstream
version. A hand-edit that bumps `pkgver` and forgets `pkgrel` publishes
`1.8.1-2` for a package that has had one revision.

## What `publish` does that is easy to get wrong

**It clones fresh, every time.** The AUR repo is not checked out anywhere
permanent in this tree, and `~/.cache/yay/xpcog` is yay's stale read-only HTTPS
cache, not it. Pushing from that is not possible and editing it is worse than
useless.

**It regenerates `.SRCINFO` inside the clone and diffs it.** The AUR serves the
`.SRCINFO` as the package's metadata and the `PKGBUILD` as its recipe; if they
disagree the page describes something the build does not produce.

**It reads the result back off the remote**, not out of the push's own output —
and off `git ls-remote` plus the package page, never `rpc/v5/info`. That endpoint
reads a metadata index aurweb rebuilds on its own schedule: it has answered with
the *previous* release for hours after the page was already current, and no
cache-buster changes that. Waiting on it just wastes the afternoon.

The package is not built here. `makepkg --verifysource` is the check;
regenerate, verify, push is the whole job. `packaging/arch/README.md` covers what
the build itself does.
