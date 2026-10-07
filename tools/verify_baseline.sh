#!/usr/bin/env bash
# verify_baseline.sh — prove that the vendored src/mpaodv (self-adaptive options removed) gives
# exactly the same PMAODV and QMAODV results as the unmodified published module.
#
# 1. fetch the pinned commit, rename its mpaodv module to `mpaodvorig` (namespace, helpers,
#    guards) in verify/ (git-ignored, never part of the project), link it into the ns-3 tree
# 2. build, run QMAODV vs QMAODV_ORIG and PMAODV vs PMAODV_ORIG (3 seeds x 2 scenarios)
# 3. compare packet counts -> IDENTICAL required
# 4. remove verify/ and the link again, rebuild
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); cd "$REPO"
source .workspace
source "$REPO/tools/ns3_common.sh"
URL=${URL:-https://github.com/Letronghien/ao-am-pm-qm-sa.git}
COMMIT=${COMMIT:-56d143cd2c27bfd9a653495ffa247c9ab23716ba}
PY=python3; [[ -x .venv/bin/python ]] && PY=.venv/bin/python
V="$REPO/verify/mpaodvorig"

RECONF=0
cleanup() {
  rm -rf "$REPO/verify"
  rm -f "$NS3/src/mpaodvorig" "$NS3/scratch/qsq-verify.cc" "$NS3"/build/scratch/*qsq-verify*
  if [[ $RECONF == 1 ]]; then   # always leave the workspace configured without the reference
    echo "== restoring the normal build (reference module removed)"
    # shellcheck disable=SC2086
    ( cd "$NS3" && ./ns3 configure $QSQ_CONFIGURE_FLAGS --enable-modules="$MODULES" >/dev/null \
                && ./ns3 build -j "$(nproc)" >/dev/null ) \
      || echo "!! rebuild failed: run 'make setup'"
  fi
}
trap cleanup EXIT

echo "== 1. fetching the published module (${COMMIT:0:12})"
TMP=$(mktemp -d)
git clone -q "$URL" "$TMP/src"; git -C "$TMP/src" checkout -q "$COMMIT"
rm -rf "$V"; mkdir -p "$REPO/verify"; cp -a "$TMP/src/mpaodv" "$V"; rm -rf "$TMP" "$V/examples"
for f in $(find "$V" -name "mpaodv-*"); do mv "$f" "$(dirname "$f")/$(basename "$f" | sed 's/^mpaodv-/mpaodvorig-/')"; done
find "$V" -type f -exec sed -i 's/mpaodv/mpaodvorig/g; s/Mpaodv/Mpaodvorig/g; s/MPAODV/MPAODVORIG/g;
  s/SaQmaodvHelper/SaQmaodvOrigHelper/g; s/PmaodvHelper/PmaodvOrigHelper/g; s/QmaodvHelper/QmaodvOrigHelper/g' {} +
sed -i '/add_subdirectory(examples)/d' "$V/CMakeLists.txt"
ln -sfn "$V" "$NS3/src/mpaodvorig"

echo "== 2. building with the reference module"
MODULES=$(qsq_modules "$REPO")
# A separate program qsq-verify.cc (a copy of qsq-compare.cc that includes the reference module
# unconditionally) is compiled for the check. Being a new file/target it is always compiled from
# scratch, which avoids stale objects (ccache / ninja) of qsq-compare. It is deleted afterwards.
{
  echo '#include "ns3/mpaodvorig-helper.h"  // verification build: reference module always present'
  echo '#define QSQ_HAVE_MPAODV_ORIG 1'
  cat "$REPO/scratch/qsq-compare.cc"
} > "$NS3/scratch/qsq-verify.cc"
RECONF=1
# shellcheck disable=SC2086
( cd "$NS3" && ./ns3 configure $QSQ_CONFIGURE_FLAGS --enable-modules="$MODULES;mpaodvorig" >/dev/null \
            && ./ns3 build -j "$(nproc)" >/dev/null )
BIN=$(ls -t "$NS3"/build/scratch/*qsq-verify* 2>/dev/null | head -1)
[[ -x "$BIN" ]] || { echo "!! verification program was not built"; exit 1; }
echo "   binary: $BIN"
# diagnostics: header present? simulator recompiled? reference protocol known?
echo "   header : $(ls "$NS3"/build/include/ns3/mpaodvorig-helper.h 2>/dev/null || echo MISSING)"
echo "   library: $(ls "$NS3"/build/lib/*mpaodvorig* 2>/dev/null | head -1 || echo MISSING)"
echo "   linked : $(ldd "$BIN" 2>/dev/null | grep -c mpaodvorig) (1 = binary uses the reference library)"
PROBE="$REPO/reports/v2/verify_probe.err"; mkdir -p "$(dirname "$PROBE")"
if ! "$BIN" --protocol=QMAODV_ORIG --seed=1 --simTime=10 --numNodes=4 --csvFile="$REPO/reports/v2/verify_probe.csv" \
       >/dev/null 2>"$PROBE"; then
  echo "!! probe run of QMAODV_ORIG failed; its error output:"
  grep -v "^ *[0-9]*# " "$PROBE" | head -15
  exit 1
fi
export LD_LIBRARY_PATH="$NS3/build/lib:${LD_LIBRARY_PATH:-}"

echo "== 3. running"
OUT="$REPO/reports/v2/verify_baseline.csv"; LOG="$REPO/reports/v2/verify_baseline_logs"
mkdir -p "$LOG"; rm -f "$OUT" "$LOG"/*
declare -A SCEN=(
  [A_N20]="--numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5"
  [M_N30_r300]="--numNodes=30 --pktInterval=0.10 --meanVelMin=15 --meanVelMax=15 --rangeM=300 --bcast11=1"
)
for tag in "${!SCEN[@]}"; do
  for s in 1 2 3; do
    for p in QMAODV QMAODV_ORIG PMAODV PMAODV_ORIG; do
      # shellcheck disable=SC2086
      "$BIN" --protocol=$p --seed=$s --simTime=200 --tag=$tag ${SCEN[$tag]} \
             --csvFile="$LOG/$p-$tag-$s.csv" >/dev/null 2>"$LOG/$p-$tag-$s.err" &
    done
    wait
  done
done
failed=$(find "$LOG" -name '*.err' -size +0 | wc -l)
if (( failed > 0 )); then
  echo "!! $failed run(s) wrote errors, first one:"; head -5 "$(find "$LOG" -name '*.err' -size +0 | head -1)"
fi
$PY -c "import glob,pandas as pd; fs=sorted(glob.glob('$LOG/*.csv')); pd.concat([pd.read_csv(f) for f in fs]).to_csv('$OUT', index=False)" \
  || { echo "no results"; exit 1; }

echo "== 4. comparison"
$PY - "$OUT" <<'PY'
import sys, pandas as pd
d = pd.read_csv(sys.argv[1]); ok = True
for base in ("QMAODV", "PMAODV"):
    a = d[d.Protocol == base].set_index(["Tag", "Seed"]); b = d[d.Protocol == base + "_ORIG"].set_index(["Tag", "Seed"])
    for k in a.index:
        if k not in b.index:
            ok = False; print(f"  {base:7s} {k[0]:14s} seed {k[1]}: reference run missing"); continue
        same = all(a.loc[k, c] == b.loc[k, c] for c in ("TxPkts", "RxPkts", "CtrlPktsAll", "CtrlBytesAll"))
        ok &= same
        print(f"  {base:7s} {k[0]:14s} seed {k[1]}: RxPkts {a.loc[k,'RxPkts']} vs original {b.loc[k,'RxPkts']}"
              f" -> {'IDENTICAL' if same else 'DIFFERENT'}")
print("RESULT:", "vendored PMAODV/QMAODV == published module" if ok else "MISMATCH - report to the assistant")
PY
echo "== 5. removing the reference module (done on exit)"
