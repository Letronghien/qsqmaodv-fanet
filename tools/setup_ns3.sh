#!/usr/bin/env bash
# setup_ns3.sh — create the project's OWN ns-3.48 tree, same layout as your other projects:
#
#   ~/qsqmaodv-repo/              <- this git repo (the only place you edit code)
#   ~/qsqmaodv-fanet/ns-3-qsq/    <- private ns-3.48 tree used to build/run
#        src/qsqmaodv  -> ~/qsqmaodv-repo/src/qsqmaodv   (symlink; also qlaodv, aomdv)
#        src/pmaodv    -> ~/qsqmaodv-repo/src/pmaodv     (symlink)
#        src/qmaodv    -> ~/qsqmaodv-repo/src/qmaodv     (symlink)
#        scratch/qsq-compare.cc -> ~/qsqmaodv-repo/scratch/qsq-compare.cc
#
# The ns-3.48 tree is a FRESH clone (your ns-3-anbq tree has local edits in the
# Wi-Fi energy model, so it is not used as a source). Other projects are never touched.
#
#   bash tools/setup_ns3.sh
#   NS3_SRC=/path/to/clean/ns-3.48 bash tools/setup_ns3.sh   # copy a local clean tree instead
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$HOME/qsqmaodv-fanet}
NS3=${NS3:-$WORK/ns-3-qsq}
source "$REPO/tools/ns3_common.sh"   # QSQ_PROFILE (release), configure flags, helpers
PROFILE=$QSQ_PROFILE
JOBS=${JOBS:-$(nproc)}

echo "repo : $REPO"
echo "ns-3 : $NS3   (profile=$PROFILE, -j$JOBS)"
mkdir -p "$WORK"

# 1. clean ns-3.48 -----------------------------------------------------------
if [[ ! -f "$NS3/VERSION" ]]; then
  if [[ -n "${NS3_SRC:-}" ]]; then
    echo "-> copying $NS3_SRC"
    rsync -a --exclude build --exclude cmake-cache --exclude '.lock-ns3*' "$NS3_SRC/" "$NS3/"
  else
    echo "-> cloning ns-3.48"
    git clone --depth 1 --branch ns-3.48 https://gitlab.com/nsnam/ns-3-dev.git "$NS3" \
      || git clone --depth 1 --branch ns-3.48 https://github.com/nsnam/ns-3-dev-git "$NS3"
  fi
fi
grep -qx "3.48" "$NS3/VERSION" || { echo "ERROR: $NS3 is not ns-3.48"; exit 1; }

# 2. baselines present in the repo? ------------------------------------------
for m in aomdv mpaodv; do
  [[ -f "$REPO/src/$m/CMakeLists.txt" ]] || { echo "-> importing baselines"; bash "$REPO/tools/import_baselines.sh"; break; }
done

# 3. link repo -> ns-3 tree --------------------------------------------------
# remove links left by modules/programs that no longer exist in the repo
for l in "$NS3"/src/* "$NS3"/scratch/*; do
  if [[ -L "$l" && ! -e "$l" ]]; then echo "   removing stale link $(basename "$l")"; rm -f "$l"; fi
done
MODULES="aodv;applications;energy;flow-monitor;internet;mobility;network;propagation;wifi"
for d in "$REPO"/src/*/; do
  d=${d%/}; m=$(basename "$d")
  [[ -f "$d/CMakeLists.txt" ]] || continue
  if [[ -e "$NS3/src/$m" && ! -L "$NS3/src/$m" ]]; then echo "ERROR: $NS3/src/$m exists and is not a link"; exit 1; fi
  ln -sfn "$d" "$NS3/src/$m"; MODULES="$MODULES;$m"; echo "   src/$m -> $d"
done
for f in "$REPO"/scratch/*.cc "$REPO"/scratch/*.h; do
  [[ -e "$f" ]] || continue
  ln -sfn "$f" "$NS3/scratch/$(basename "$f")"; echo "   scratch/$(basename "$f")"
done

# 4. include-guard check (modules cloned from AODV often share guards) ---------
REPO_MODS=$(for d in "$REPO"/src/*/; do [[ -f "$d/CMakeLists.txt" ]] && basename "$d"; done | tr '\n' ' ')
GUARD_LOG="$REPO/docs/BASELINE_PROVENANCE.md" \
python3 "$REPO/tools/check_guards.py" --fix="$(echo $REPO_MODS | tr ' ' ',')" "$NS3" aodv $REPO_MODS || {
  echo "ERROR: fix the collisions above (or send this output to the assistant)"; exit 1; }

# 5. configure + build ---------------------------------------------------------
cd "$NS3"
# shellcheck disable=SC2086
./ns3 configure $QSQ_CONFIGURE_FLAGS --enable-modules="$MODULES"
./ns3 build -j "$JOBS"

echo "NS3=$NS3" > "$REPO/.workspace"

# 6. check that the baseline attributes used by qsq-compare exist -------------
BIN=$(qsq_binary "$NS3")
export LD_LIBRARY_PATH="$NS3/build/lib:${LD_LIBRARY_PATH:-}"
echo; echo "== attribute check (qsq-compare sets these on the baselines) =="
check_attrs() {   # $1 = TypeId, rest = attribute names
  local tid=$1; shift
  local out; out=$("$BIN" --PrintAttributes="$tid" 2>&1 || true)
  if ! grep -q -- "--$tid::" <<<"$out"; then
    echo "  TypeId $tid not found. Registered TypeIds containing 'maodv':"
    "$BIN" --PrintTypeIds 2>/dev/null | grep -i maodv | sed 's/^/     /'
    return
  fi
  for a in "$@"; do
    if grep -q -- "::$a=\[" <<<"$out"; then echo "  OK      $tid::$a"
    else echo "  MISSING $tid::$a"; fi
  done
}
check_attrs ns3::mpaodv::RoutingProtocol Policy MaxPaths Alpha0 Gamma Epsilon0 RewardW1 RewardW2
check_attrs ns3::aomdv::RoutingProtocol MaxPaths HelloInterval ActiveRouteTimeout
check_attrs ns3::qsqmaodv::RoutingProtocol HopByHop QueueRewardWeight FailurePenalty QueueAwareSelect QueueDrivenExploration ExplorationFloor AckSilenceDecay QueueRefPackets
check_attrs ns3::qlaodv::RoutingProtocol QlAlpha QlGamma QlEpsilon0 QlEpsilonMin QlEpsilonDecay QlMaxRoutes QlRrepWait
echo; echo "Done. Next:  cd $REPO && make smoke"
