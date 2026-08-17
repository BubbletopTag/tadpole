#!/bin/bash
# Tadpole/Glasspole — cut a release, with BOTH platforms' assets on it.
#
#   ./tools/release.sh                 build both, tag, publish
#   ./tools/release.sh --dry-run       say what would happen; touch nothing
#   ./tools/release.sh --tag tadpole-09082026-0004
#   ./tools/release.sh --allow-stale   ship an AppImage older than the commit
#
# THIS IS THE ONLY SCRIPT THAT CUTS A RELEASE, and it did not used to be.
# ------------------------------------------------------------------------
# There were two. post-commit.sh — untracked, living in the maintainer's
# working copy — attached the Linux AppImage only, but it stamped the viewer
# with the release version and then re-read the binary with `strings` to prove
# the stamp had taken. This script attached both platforms' assets and did
# neither. Both were run, at different times, and the safety properties were
# split down the middle: whichever you used, you gave something up.
#
# What that cost is on the releases page. tadpole-10082026-0007 was cut with
# this script, and the AppImage on it contains the literal string "dev" and no
# version at all — so the current release tells every Linux user it is an
# unreleased build, at every launch, and its update check can never say "up to
# date". The Windows half was fine, because tools/build-windows.sh compiles the
# viewer from source with -DTADPOLE_VERSION on the command line every time.
#
# So the two are merged here rather than kept in step by hand, and this script
# now carries everything post-commit.sh had:
#
#   * the sequence number computed from local tags AND `gh release list`
#   * stamp-then-verify, for BOTH platforms, before anything is tagged
#   * the stale-image refusal
#   * draft-first publishing, so a failed upload leaves nothing to find
#   * the asset size read back from GitHub before the draft goes public
#   * --latest on publish
#   * the tag pushed by git, with --verify-tag, never invented server-side
#
# post-commit.sh should now be one line — `exec ./tools/release.sh "$@"` — and
# the flags it took are all accepted here. It stays out of git for the reason
# .gitignore gives: it publishes to one particular repository and is the
# maintainer's tool rather than part of what Tadpole is.
#
# The release BODY is the commit message, unchanged: that is the contract those
# messages have been written to all along.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
cd "$PROJ"

die() { echo "error: $*" >&2; exit 1; }

DRY=0
TAG=""
ALLOW_STALE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run|-n)  DRY=1 ;;
        --tag)         shift; TAG="${1:-}" ;;
        --allow-stale) ALLOW_STALE=1 ;;
        # post-commit.sh's flag. Building is what this script does, so there is
        # nothing to turn on; accepted so the old muscle memory still works.
        --build)       ;;
        -h|--help)     sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

command -v gh >/dev/null || die "gh (the GitHub CLI) is not installed"
gh auth status >/dev/null 2>&1 || die "gh is installed but not logged in — run: gh auth login"

REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null)" \
    || die "gh cannot tell which GitHub repository this is — check 'git remote -v'"

SHA="$(git rev-parse HEAD)"
SHORT="$(git rev-parse --short HEAD)"
SUBJECT="$(git log -1 --format=%s)"
COMMIT_EPOCH="$(git log -1 --format=%ct)"

# ---- the version -----------------------------------------------------------
# tadpole-DDMMYYYY-NNNN, the scheme tools/check-update.py parses. NNNN counts
# releases made today.
#
# BOTH SOURCES, because neither is complete on its own: a local tag may not be
# pushed, and a release cut from another machine has no local tag until you
# fetch. This used to be `git tag -l | wc -l`, which is worse in three separate
# ways — it counted tags rather than reading their sequence numbers, so one
# missing tag silently reused a number; it ignored published releases entirely;
# and a count is not a maximum, so deleting an old tag moved the next release
# backwards.
#
# A failure to list is fatal rather than "assume none": guessing -0001 when the
# answer was -0003 does not produce a wrong number, it produces a collision.
if [ -z "$TAG" ]; then
    DATE="$(date +%d%m%Y)"
    RELEASED="$(gh release list --limit 200 --json tagName -q '.[].tagName')" \
        || die "could not list the existing releases of $REPO, so the next
  sequence number cannot be worked out. Check the network and 'gh auth status'."
    KNOWN="$(git tag -l "tadpole-$DATE-*")
$RELEASED"
    high=0
    for t in $KNOWN; do
        case "$t" in
            tadpole-"$DATE"-[0-9][0-9][0-9][0-9])
                # 10# or bash reads 0008 as octal and errors out on the 8.
                n=$((10#${t##*-}))
                [ "$n" -gt "$high" ] && high="$n" ;;
        esac
    done
    [ "$high" -ge 9999 ] && die "9999 releases today already — no room left"
    TAG="$(printf 'tadpole-%s-%04d' "$DATE" "$((high + 1))")"
fi
VERSION="${TAG#tadpole-}"
# The tag is the same string lowercased and hyphen-joined; the title is spaced,
# because a git ref cannot contain a space. check-update.py parses either.
TITLE="Tadpole ${VERSION}"

git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && \
    die "tag $TAG already exists locally but was not counted — refusing to guess.
  Look at it:  git show $TAG        Or drop it:  git tag -d $TAG"

echo "==> $TITLE  ($TAG)"
echo "    $SHORT  $SUBJECT"

# Uncommitted changes are the mirror image of a stale image: the binaries would
# carry code the release notes' commit does not. A warning rather than a
# refusal, because build/ and the firmware directories are full of untracked
# working files and stopping for those would be wrong.
DIRTY="$(git status --porcelain --untracked-files=no)"
[ -n "$DIRTY" ] && {
    echo "WARNING: tracked files are modified and are NOT in $SHORT:" >&2
    echo "$DIRTY" | sed 's/^/    /' >&2
}

# ---- the tests that need no build -------------------------------------------
# There is no CI here, so a test that is not run on the way to a release is a
# test that is not run. These are the host-side rules that Linux cannot
# otherwise exercise — the Windows path and symlink handling — and they cost
# under a second between them. A release is the last moment they are free.
echo "==> host-side tests"
if [ -f "$HERE/tests/link_resolve_test.py" ]; then
    python3 "$HERE/tests/link_resolve_test.py" \
        || die "the rootfs symlink rules are wrong — refusing to ship an
  installer that would fail partway through building a sysroot"
fi

# ---- build both ------------------------------------------------------------
# NOTHING IS TAGGED OR PUSHED UNTIL BOTH ASSETS EXIST AND BOTH CARRY $VERSION.
# A release is two files; producing one of them and discovering the other
# cannot be built is a state worth never being in.
echo "==> Linux AppImage"
# build-appimage.sh reads TADPOLE_VERSION, runs `make viewer` with it and
# verifies the stamp with `strings` before packaging. It used to accept this
# variable and discard it, which is how the "dev" release happened.
TADPOLE_VERSION="$VERSION" "$HERE/build-appimage.sh" \
    || die "the AppImage did not build — nothing has been tagged or pushed"
APPIMAGE="$(ls -1 "$PROJ"/build/Tadpole-x86_64.AppImage \
                  "$PROJ"/Tadpole-x86_64.AppImage 2>/dev/null | head -1 || true)"
[ -n "$APPIMAGE" ] || die "no AppImage was produced — nothing has been tagged or pushed"

echo "==> Windows installer"
"$HERE/build-windows.sh" --installer --version "$VERSION" \
    || die "the Windows installer did not build — nothing has been tagged or pushed"
SETUP="$PROJ/build/win/Glasspole-Setup.exe"
[ -f "$SETUP" ] || die "no installer at $SETUP"

# ---- the assets are the release --------------------------------------------
# An AppImage is an ELF whose bytes 8..9 are the magic 'AI'. Cheap, and it
# catches the one mistake that would otherwise be found by whoever downloads it.
read -r magic < <(od -An -c -N 10 "$APPIMAGE" | tr -d ' ')
case "$magic" in
    '177ELF'*'AI') ;;
    *) die "$APPIMAGE does not look like an AppImage (bad magic)" ;;
esac

# BELT AND BRACES ON THE VERSION. Each build script already refuses to produce
# an unstamped binary, but this is the last point at which a release can be
# stopped for free, and the failure it guards against — shipping a build that
# misreports itself — is silent, permanent for that release, and was live in
# tadpole-10082026-0007. Read it out of the finished artifacts.
# `grep -Fx ... >/dev/null` rather than `grep -qx`: this script runs under
# `set -o pipefail`, and -q exits on the first match while `strings` is still
# writing, so the pipeline reports the SIGPIPE (141) and the check fails on a
# binary that is perfectly correct. Reading to the end is the whole fix.
echo "==> confirming both assets carry $VERSION"
strings "$PROJ/tadpole/viewer/tadpole-view" 2>/dev/null | grep -Fx -- "$VERSION" >/dev/null \
    || die "the viewer packaged into the AppImage does not carry $VERSION"
strings "$SETUP" 2>/dev/null | grep -Fx -- "$VERSION" >/dev/null \
    || die "$SETUP does not carry $VERSION"
echo "    both report $VERSION"

# THE STALE IMAGE. The one genuinely dangerous failure mode is shipping
# yesterday's binary under today's release notes. Both are rebuilt above, so
# this should never fire; it is here because "should never" is what it was
# last time too.
APPIMAGE_EPOCH="$(stat -c %Y "$APPIMAGE")"
if [ "$APPIMAGE_EPOCH" -lt "$COMMIT_EPOCH" ] && [ "$ALLOW_STALE" = 0 ]; then
    die "the AppImage is OLDER than the commit being released.
      built  $(date -d "@$APPIMAGE_EPOCH" '+%Y-%m-%d %H:%M')
      commit $SHORT made $(date -d "@$COMMIT_EPOCH" '+%Y-%m-%d %H:%M')
  Releasing it would ship a binary that does not contain $SHORT, described by
  $SHORT's message.  --allow-stale ships it anyway."
fi

APPIMAGE_BYTES="$(stat -c %s "$APPIMAGE")"
SETUP_BYTES="$(stat -c %s "$SETUP")"
ls -l "$APPIMAGE" "$SETUP"

# ---- the release notes -----------------------------------------------------
# The commit message verbatim, minus its trailers — those are provenance for
# the repository, not something a stranger reading a release page needs, and
# the session URL in particular has no business on a public page.
NOTES="$(mktemp "${TMPDIR:-/tmp}/tadpole-notes.XXXXXX")" || die "cannot create a temporary file"
trap 'rm -f "$NOTES"' EXIT
git log -1 --format=%B |
    sed -E '/^(Co-Authored-By|Co-authored-by|Signed-off-by|Claude-Session|Generated-with):/d' |
    sed -E ':a; /^[[:space:]]*$/{ $d; N; ba; }' > "$NOTES"

if [ "$DRY" = 1 ]; then
    cat <<EOF

==> DRY RUN — nothing has been tagged, pushed or published

  repository   $REPO
  commit       $SHORT  $SUBJECT
  tag          $TAG            (annotated, created at $SHORT)
  title        $TITLE
  assets       Tadpole-x86_64.AppImage  $APPIMAGE_BYTES bytes
               Glasspole-Setup.exe      $SETUP_BYTES bytes
               both verified to carry $VERSION
  would run    git push origin refs/tags/$TAG
               gh release create $TAG --verify-tag --draft ...
               gh release view $TAG --json assets    (must list both)
               gh release edit $TAG --draft=false --latest

  release notes, verbatim from $SHORT:
EOF
    printf -- '---8<---------------------------------------------------------------\n'
    cat "$NOTES"
    printf -- '--->8---------------------------------------------------------------\n'
    exit 0
fi

# ---- tag, push the tag, publish --------------------------------------------
# The tag is created locally at HEAD and pushed with git BEFORE `gh release
# create` runs, and --verify-tag makes gh refuse rather than invent one. gh
# would otherwise create a missing tag server-side at the tip of the default
# branch — which, since the commits are pushed separately, is the PREVIOUS
# commit. That publishes a release whose tag points at the wrong code,
# described by notes taken from a commit it does not contain.
git tag -a "$TAG" -m "$TITLE" "$SHA" || die "could not create the tag $TAG"

echo "==> pushing the tag (only the tag)"
if ! git push origin "refs/tags/$TAG"; then
    # Nothing else can reference a tag made two seconds ago, so removing it is
    # safe — and leaving it behind would silently consume today's sequence
    # number, so the next attempt would skip one.
    git tag -d "$TAG" >/dev/null 2>&1
    die "could not push $TAG to origin — the local tag has been removed again."
fi

CLEANUP="  gh release delete $TAG --yes --cleanup-tag
  git tag -d $TAG
  git push --delete origin $TAG"

# A DRAFT until both assets are confirmed to be on it. Drafts are not returned
# by the public releases API and do not appear on the releases page, so an
# upload that fails here leaves nothing for anyone — including the in-app
# update checker — to find. Publishing first and fixing afterwards would mean a
# window in which the newest release is one that cannot be downloaded, and on
# Windows that window is worse than it sounds: check-update.py looks for
# Glasspole-Setup.exe by name and reports the update as missing without it.
echo "==> creating a draft release and uploading both assets"
gh release create "$TAG" --verify-tag --draft \
    --title "$TITLE" --notes-file "$NOTES" \
    "$APPIMAGE" "$SETUP" \
    || die "gh release create failed — nothing has been published.
  The tag IS on GitHub now. Remove it before trying again:
$CLEANUP"

# Do not take the upload's word for it. "The command exited 0" and "the file is
# attached and complete" are not the same claim.
echo "==> checking GitHub really has them"
attached_bytes() {
    gh release view "$TAG" --json assets \
        -q ".assets[] | select(.name == \"$1\") | .size" 2>/dev/null
}
for pair in "Tadpole-x86_64.AppImage:$APPIMAGE_BYTES" "Glasspole-Setup.exe:$SETUP_BYTES"; do
    name="${pair%:*}"; want="${pair##*:}"
    got="$(attached_bytes "$name")"
    if [ -z "$got" ]; then
        echo "    $name is not attached — retrying the upload once" >&2
        gh release upload "$TAG" \
            "$([ "$name" = Glasspole-Setup.exe ] && echo "$SETUP" || echo "$APPIMAGE")" \
            --clobber || true
        got="$(attached_bytes "$name")"
    fi
    [ -n "$got" ] || die "$name did not attach, so the release is still an
  unpublished DRAFT and nobody can see it — which is the right outcome, because
  a release missing one platform's asset tells that platform there is no update.
$CLEANUP"
    [ "$got" = "$want" ] || die "$name attached but is the wrong size:
      GitHub has $got bytes, this machine has $want. The upload was truncated
      and the release is still an unpublished DRAFT.
$CLEANUP"
    echo "    $name  $got bytes  attached and complete"
done

# --latest explicitly: the update checker asks for the newest release, and
# GitHub would otherwise decide by semver-ish tag ordering, which these date
# tags are not.
echo "==> publishing"
gh release edit "$TAG" --draft=false --latest >/dev/null \
    || die "both assets are attached but the release is still a DRAFT — publish
  it by hand:  gh release edit $TAG --draft=false --latest"

echo
echo "Released: https://github.com/$REPO/releases/tag/$TAG"
echo
echo "  assets      Tadpole-x86_64.AppImage, Glasspole-Setup.exe — both $VERSION"
echo "  pushed      refs/tags/$TAG  (and the objects it reaches)"
echo "  NOT pushed  your branch — run 'git push' yourself when you are ready"
