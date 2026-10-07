#!/usr/bin/env bash
# make_diffs.sh — show exactly what the proposed and re-implemented protocols change.
#   docs/diffs/qsqmaodv_vs_mpaodv.diff : QS-QMAODV versus the mpaodv module (QMAODV/PMAODV) it was
#                                        forked from (the baseline helper classes are removed in the fork)
#   docs/diffs/qlaodv_vs_aodv.diff     : QL-AODV re-implementation versus ns-3.48 AODV
# The baseline is renamed exactly like the fork (identifiers, file names) so that the diff
# contains only real changes.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
[[ -f "$REPO/.workspace" ]] && source "$REPO/.workspace"
OUT="$REPO/docs/diffs"; mkdir -p "$OUT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

fork() {  # $1 src dir  $2 old  $3 new  $4 comment-family
  local d="$TMP/a/$3"; mkdir -p "$TMP/a"; cp -r "$1" "$d"
  rm -rf "$d/test" "$d/examples" "$d/doc" "$d/CMakeLists.txt"
  for f in $(find "$d" -name "$2-*"); do mv "$f" "$(dirname "$f")/$(basename "$f" | sed "s/^$2-/$3-/")"; done
  local O=${2^^} N=${3^^} o1=${2^} n1=${3^}
  find "$d" -type f -exec sed -i "s/$2/$3/g; s/$o1/$n1/g; s/$O/$N/g" {} +
  python3 "$REPO/tools/restore_comments.py" "$4" $(find "$d" -name "*.h" -o -name "*.cc")
}
cmp_dir() {  # $1 module
  mkdir -p "$TMP/b"; cp -r "$REPO/src/$1" "$TMP/b/$1"; rm -f "$TMP/b/$1/CMakeLists.txt"
}

fork "$REPO/src/mpaodv" mpaodv qsqmaodv none; cmp_dir qsqmaodv
rm -f "$OUT/qsqmaodv_vs_qmaodv.diff"
(cd "$TMP" && diff -ruN a/qsqmaodv b/qsqmaodv > "$OUT/qsqmaodv_vs_mpaodv.diff" || true)

if [[ -n "${NS3:-}" && -d "$NS3/src/aodv" ]]; then
  fork "$NS3/src/aodv" aodv qlaodv qlaodv; cmp_dir qlaodv
  (cd "$TMP" && diff -ruN a/qlaodv b/qlaodv > "$OUT/qlaodv_vs_aodv.diff" || true)
fi
# what tools/strip_sa.py removed from the published module (needs network)
URL=https://github.com/Letronghien/ao-am-pm-qm-sa.git; COMMIT=56d143cd2c27bfd9a653495ffa247c9ab23716ba
if git clone -q "$URL" "$TMP/pub" 2>/dev/null && git -C "$TMP/pub" checkout -q "$COMMIT"; then
  (cd "$TMP" && diff -ruN pub/mpaodv "$REPO/src/mpaodv" | sed "s#$REPO/src/#vendored/#" > "$OUT/mpaodv_vendored_vs_published.diff" || true)
fi
for f in "$OUT"/*.diff; do
  printf "%-32s %5d lines changed in %d files\n" "$(basename "$f")" \
    "$(grep -c '^[+-][^+-]' "$f")" "$(grep -c '^+++ ' "$f")"
done
