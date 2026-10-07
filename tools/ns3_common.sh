# ns3_common.sh — shared settings, sourced by setup_ns3.sh, verify_baseline.sh and run_jobs.sh.
# One place for the configure flags so that every (re)configuration uses the same build profile.
QSQ_PROFILE=${PROFILE:-release}
QSQ_CONFIGURE_FLAGS="--build-profile=$QSQ_PROFILE --disable-examples --disable-tests --disable-python-bindings --disable-werror"

# modules of this project: ns-3 modules used by the simulator + every module in src/
qsq_modules() {
  local repo=$1 m="aodv;applications;energy;flow-monitor;internet;mobility;network;propagation;wifi"
  for d in "$repo"/src/*/; do [[ -f "$d/CMakeLists.txt" ]] && m="$m;$(basename "$d")"; done
  echo "$m"
}

# the simulation binary of the configured profile (release has no suffix)
qsq_binary() {
  local ns3=$1 suffix=""
  [[ "$QSQ_PROFILE" != release ]] && suffix="-$QSQ_PROFILE"
  local b="$ns3/build/scratch/ns3.48-qsq-compare$suffix"
  [[ -x "$b" ]] && { echo "$b"; return; }
  # fallback: newest matching binary
  ls -t "$ns3"/build/scratch/*qsq-compare* 2>/dev/null | head -1
}
