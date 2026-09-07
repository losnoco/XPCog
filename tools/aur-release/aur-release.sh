#!/usr/bin/env bash
#
# Take a published XPCog release to the AUR.
#
# Two steps, because they touch two repositories and only the second is
# irreversible:
#
#   tools/aur-release/aur-release.sh prepare [version]
#       Rewrites packaging/arch/PKGBUILD and .SRCINFO for the release and
#       verifies them. Leaves both uncommitted for review.
#
#   tools/aur-release/aur-release.sh publish [version] [-m message]
#       Clones the AUR repo fresh, copies those two files in, commits and
#       pushes, then reads the result back off the remote. The message is what
#       shows on the AUR's page, so it is written for people who install the
#       package -- what changed in the player, not what changed in the
#       packaging. Defaults to the GitHub release's own title.
#
# `prepare` refuses to run before the GitHub release exists, which is the whole
# reason this is not part of CI: the tarball it checksums is published by the
# release job, so doing this any earlier hashes a 404.
#
# The checksum is taken by hand rather than with updpkgsums, which rewrites the
# whole sha256sums array in its own layout and replaces the vcpkg entry's SKIP
# with a hash of the git tree it happened to fetch -- not reproducible, and
# contradicting the comment above the array. Only the first entry is touched
# here; SKIP and the multi-line layout are left exactly as they are.
#
# Environment: AUR_REMOTE (default ssh://aur@aur.archlinux.org/xpcog.git).

set -euo pipefail

readonly REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly ARCH_DIR="$REPO_ROOT/packaging/arch"
readonly AUR_REMOTE="${AUR_REMOTE:-ssh://aur@aur.archlinux.org/xpcog.git}"

usage() {
    sed -n '3,28p' "$0" | sed 's|^#\s\?||'
    exit 2
}

die() { echo "aur-release: $*" >&2; exit 1; }

# The tree's own version, so the common case takes no argument at all.
tree_version() {
    sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9.]\+\)[[:space:]]*$/\1/p' \
        "$REPO_ROOT/CMakeLists.txt" | head -1
}

do_prepare() {
    local version="$1"
    local work; work="$(mktemp -d)"
    trap 'rm -rf "$work"' RETURN

    echo "release:   checking v$version is published"
    gh release view "v$version" --json tagName,publishedAt \
        --jq '"           " + .tagName + "  " + .publishedAt' \
        || die "no release v$version yet -- wait for CI's release job"

    local url="https://github.com/losnoco/XPCog/archive/refs/tags/v$version.tar.gz"
    echo "tarball:   $url"
    curl -fsSL -o "$work/src.tar.gz" "$url" || die "could not download the tarball"
    local sum; sum="$(sha256sum "$work/src.tar.gz" | cut -d' ' -f1)"
    echo "sha256:    $sum"

    # Against the *released* tree, not the working one: what the PKGBUILD's
    # prepare() will compare is what is inside that tarball.
    local pin baseline
    pin="$(sed -n 's/^_vcpkg_commit=\(.*\)$/\1/p' "$ARCH_DIR/PKGBUILD")"
    baseline="$(tar xzOf "$work/src.tar.gz" "XPCog-$version/vcpkg.json" \
        | sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p')"
    if [ "$pin" != "$baseline" ]; then
        die "_vcpkg_commit is $pin but the released vcpkg.json says $baseline"
    fi
    echo "vcpkg:     $pin (matches the released manifest)"

    # pkgver, and only the first checksum. pkgrel goes back to 1: it counts
    # packaging revisions within one upstream version, so a new version resets it.
    sed -i "s/^pkgver=.*/pkgver=$version/; s/^pkgrel=.*/pkgrel=1/" "$ARCH_DIR/PKGBUILD"
    sed -i "0,/^  '[0-9a-f]\{64\}'$/s//  '$sum'/" "$ARCH_DIR/PKGBUILD"
    grep -q "'$sum'" "$ARCH_DIR/PKGBUILD" || die "the checksum line was not replaced"
    grep -q "'SKIP'" "$ARCH_DIR/PKGBUILD" || die "the vcpkg SKIP entry was lost"

    echo "verify:    makepkg --verifysource"
    ( cd "$ARCH_DIR" && makepkg --verifysource ) || die "verifysource failed"

    ( cd "$ARCH_DIR" && makepkg --printsrcinfo > .SRCINFO )
    echo "srcinfo:   regenerated"

    echo
    echo "Prepared $version. Review, commit as:"
    echo "    Update the AUR package to $version [skip ci]"
    echo "then: tools/aur-release/aur-release.sh publish $version"
}

do_publish() {
    local version="$1"
    local work; work="$(mktemp -d)"
    trap 'rm -rf "$work"' RETURN

    grep -q "^pkgver=$version$" "$ARCH_DIR/PKGBUILD" \
        || die "packaging/arch/PKGBUILD is not at $version; run prepare first"

    echo "clone:     $AUR_REMOTE"
    git clone --quiet "$AUR_REMOTE" "$work/aur" || die "could not clone the AUR repo"

    cp "$ARCH_DIR/PKGBUILD" "$ARCH_DIR/.SRCINFO" "$work/aur/"

    # The .SRCINFO has to be the one this PKGBUILD generates, or the AUR shows
    # metadata that does not match the recipe it serves.
    ( cd "$work/aur" && diff <(makepkg --printsrcinfo) .SRCINFO ) \
        || die ".SRCINFO does not match the PKGBUILD"
    echo "srcinfo:   matches the PKGBUILD"

    if ( cd "$work/aur" && git diff --quiet ); then
        echo "nothing to push: the AUR is already at $version"
        return 0
    fi

    # Written for users of the package, not for whoever reads the packaging
    # diff: this message is what shows on the AUR's page.
    local message="$2"
    if [ -z "$message" ]; then
        message="$(gh release view "v$version" --json name --jq .name 2>/dev/null || true)"
    fi
    [ -n "$message" ] || die "no message: pass -m, or give the release a title"

    ( cd "$work/aur" \
        && git add PKGBUILD .SRCINFO \
        && git commit --quiet -m "$message" \
        && git push --quiet origin master ) || die "the push failed"

    echo "pushed:    $message"
    echo "remote:    $(git ls-remote "$AUR_REMOTE" master | cut -c1-12)"
    # The package page, not rpc/v5/info -- that index is rebuilt on aurweb's own
    # schedule and has been hours stale while the page was already current.
    echo "page:      $(curl -fsSL https://aur.archlinux.org/packages/xpcog \
                        | grep -o 'xpcog [0-9.]*-[0-9]*' | head -1)"
}

command="${1:-}"
shift || true

version=""
message=""
while [ $# -gt 0 ]; do
    case "$1" in
        -m) message="${2:-}"; shift 2 || usage ;;
        -*) usage ;;
        *)  version="$1"; shift ;;
    esac
done
[ -n "$version" ] || version="$(tree_version)"
[ -n "$version" ] || die "could not read the version from CMakeLists.txt"

case "$command" in
    prepare) do_prepare "$version" ;;
    publish) do_publish "$version" "$message" ;;
    *)       usage ;;
esac
