#!/usr/bin/env bash
# bootstrap_new_repo.sh — turn this folder into a BRAND-NEW git repository
# (fresh history, first commit by you) and push it to your new, empty GitHub repo.
#
#   cd ~/qsqmaodv-repo
#   bash tools/bootstrap_new_repo.sh git@github.com:Letronghien/<new-repo>.git
#
# Steps: import PMAODV/QMAODV from ~/nbqmaodv-fanet/ns-3-nbq/src (read-only),
#        git init (branch main), first commit, push.
# Old data in data/legacy_v1 are kept only as reference input; all paper results
# will be regenerated into data/v2 with ns-3.48.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
URL=${1:-}
[[ -n "$URL" ]] || { echo "usage: $0 git@github.com:<user>/<repo>.git"; exit 1; }
cd "$REPO"

if [[ -d .git ]]; then
  echo "ERROR: $REPO is already a git repository. This script is only for the first setup."; exit 1
fi

# 1. baselines (read-only copy from the NBQ-MAODV ns-3.48 tree)
BASE_SRC=${BASE_SRC:-$HOME/nbqmaodv-fanet/ns-3-nbq/src} bash tools/import_baselines.sh

# 2. identity check
git config --global user.name  >/dev/null || { echo "set: git config --global user.name  \"Le Trong Hien\""; exit 1; }
git config --global user.email >/dev/null || { echo "set: git config --global user.email \"you@example.com\""; exit 1; }

# 3. fresh repository
git init -q -b main
git add -A
git commit -q -m "Initial commit: QS-QMAODV v2 on ns-3.48 (QS-QMAODV + baselines)"
git remote add origin "$URL"
echo "-> first commit: $(git log --oneline -1)"

# 4. push (handles a GitHub repo that was created with a README/LICENSE)
if git ls-remote --exit-code --heads origin >/dev/null 2>&1; then
  echo "-> remote already has commits (README/LICENSE?) — merging them first"
  git fetch -q origin
  RB=$(git ls-remote --symref origin HEAD | awk '/^ref:/{sub("refs/heads/","",$2); print $2}')
  RB=${RB:-main}
  git merge -q --allow-unrelated-histories -X ours "origin/$RB" -m "Merge initial GitHub files"
  [[ "$RB" != main ]] && git branch -m main "$RB" && echo "   (using remote default branch '$RB')"
fi
git push -u origin "$(git branch --show-current)"
echo
echo "Done. Repository: $URL"
echo "Next: make setup   (creates ~/qsqmaodv-fanet/ns-3-qsq and builds)"
