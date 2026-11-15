#!/bin/sh
set -eu

cd -- "$(dirname -- "$0")/.."

upstream_url=https://gitlab.gnome.org/GNOME/nautilus.git
upstream_ref=refs/vinder/nautilus-upstream
fork_point=d3383fcb88fd68ab19296e7df35d783bd5395bc7

git fetch --no-tags --no-write-fetch-head "$upstream_url" "HEAD:$upstream_ref"

if ! git merge-base --is-ancestor "$fork_point" HEAD ||
   ! git merge-base --is-ancestor "$fork_point" "$upstream_ref"; then
    echo "Cannot compare Vinder and Nautilus from their recorded fork point." >&2
    exit 1
fi

printf '\nUpstream commits to review against %s\n' "$(git rev-parse --short HEAD)"
git log --right-only --cherry-pick --no-merges --reverse \
    --format='%h %s' --name-only "HEAD...$upstream_ref"
