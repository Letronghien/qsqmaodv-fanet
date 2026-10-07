#!/usr/bin/env bash
# make_release.sh — archive of the committed state for the journal / Zenodo.
# Uses `git archive`, so only committed files are included and the paths marked
# export-ignore in .gitattributes (legacy/, docs/internal/) are left out.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); cd "$REPO"
git diff --quiet && git diff --cached --quiet || { echo "commit your changes first"; exit 1; }
TAG=$(git describe --tags --always)
OUT="$REPO/release/qsqmaodv-fanet-$TAG.zip"; mkdir -p release
git archive --format=zip --prefix="qsqmaodv-fanet-$TAG/" -o "$OUT" HEAD
echo "release archive: $OUT ($(du -h "$OUT" | cut -f1))"
unzip -l "$OUT" | tail -1
